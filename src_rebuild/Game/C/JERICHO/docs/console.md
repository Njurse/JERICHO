# jer_console — the unified status console

A small ring of short status lines drawn **half-size at the bottom left**, in the
frontend and in-game, newest at the bottom. It exists so the messages that used to
be scattered — mp's connection/handshake events, and whatever else a module wants
to report — land in **one** place instead of three.

```
[mp] connected 192.168.1.5:1318
[mp] sent HELLO 192.168.1.5:1318
[mp] WELCOME received, awaiting the level
[mp] handshake done
[mp] match started
```

Implementation: `src_rebuild/Game/C/jer_console.c` (engine-side, like `jer_hud.c`,
because only the game can draw into the display buffer). Header:
`JERICHO/include/jer_console.h`, mirrored at `JERICHO/sdk/include/jer_console.h`.

## API

```c
/* status lines -- single colour, dimmed with age, hidden when the console is off */
void jer_console_line(const char* text);          /* one ready-made line   */
void jer_console_log(const char* fmt, ...);       /* printf-style          */

/* chat lines -- built from colour runs, drawn EVEN WHILE the console is off */
typedef struct JER_CONSOLE_SEG {
    const char* text;
    unsigned char r, g, b;
    int ambient;          /* non-zero = keep the plain line colour */
} JER_CONSOLE_SEG;
void jer_console_chat(const JER_CONSOLE_SEG* segs, int count);

/* the line being typed (the chat prompt) -- always drawn while set */
void jer_console_input(const char* text);

void jer_console_clear(void);                     /* empty the ring        */
int  jer_console_count(void);                     /* rows currently held   */

int  jer_console_enabled(void);                   /* is the console shown? */
void jer_console_set_enabled(int on);             /* set + SAVE            */
int  jer_console_toggle(void);                    /* flip + SAVE, returns the new state */
```

A line is **kept** (scrollback), not flashed: the join sequence runs in the
frontend, and the same ring is drawn there too, so a "connected" line is still on
screen once the level is up. Text longer than a row is **wrapped** onto the next
row (breaking at a space near the limit) instead of running off the edge, and the
ring is `JER_CONSOLE_MAX` (8) rows of `JER_CONSOLE_LINE_MAX` (96) bytes; the
oldest falls off.

Every line is also mirrored to the session log as `[console] <text>`, so whatever
is on screen is greppable exactly like `MpConnEvent`'s own lines.

## The `~` toggle

`~` / backtick flips the console and **saves** the choice (`jer_config` store:
`CONFIG/hud.ini`, key `console`, default ON). The key is handled in
`redriver2_psxpc.cpp`, which wraps PsyX's single debug-key slot
(`g_dbg_gameDebugKeys`) instead of claiming it, so mp's chat handler still works;
the toggle fires once per key **press**, not per auto-repeat.

While the console is OFF the whole **bottom HUD block** is hidden — the stream
*and* mp's `HOST`/`CLIENT` tag and its two connection lines. Modules that own part
of that block ask `jer_console_enabled()` before drawing:

```c
if (jer_console_enabled())
{
    /* HOST / CLIENT tag + live connection lines */
}
```

**Chat is the exception.** A chat line and the input line are drawn even while
the console is off — a conversation is not debug output, and what you are typing
must never be invisible. That is why a chat row carries its own `chat` flag
rather than being just another status line.

## Drawing

Two draw sites, because the two states use different fonts:

| state | call | font |
| --- | --- | --- |
| in-game | `jer_console_draw(0)` in `DrawGame` (`main.c`) | `PrintStringHiresScaledSpaced`, scale `0.138f` (half the `0.275f` default) + `0.75f` px tracking |
| frontend | `jer_console_draw(1)` in `State_FrontEnd` (`FEmain.c`) | `FEPrintStringSized`, scale `2048` (half the frontend's `4096`) |

Layout is bottom-left: `x = 6`, newest row at `y = 196`, rows `11` px apart
(frontend `x = 32`, `y = 456`, rows `18` px). The typed chat line sits under the
block at `y = 232` (`496` in the frontend).

The in-game text carries a little **tracking** (`JER_CONSOLE_TRACKING`): at half
size the HQ font's own side bearings collapse to under a pixel (measured: a
4.50 px `M` advance with a 3.9 px glyph — a 0.6 px gap, about half what the
default text has), so the letters read as jammed. `PrintStringHiresScaledSpaced`
adds the extra per-advance width; the frontend's larger font does not need it.

Without an HQ font loaded the in-game path falls back to the plain PSX printer.
Both draw sites are single-view only, exactly like `jer_hud_draw` and the
`JER_EVENT_DRAW_OVERLAY` block (mp's own HUD): `NumPlayers > 1` is LOCAL
split-screen, which neither the mp mod nor these HUDs use.

`jer_console_draw` saves and restores `gFontColour`, like `jer_hud.c`: the module
overlays drawn after it expect the colour they left behind.

## Related

The bar captions (`Damage` / `Felony` / `Proximity`, see `DrawBarTag` in
`overlay.c`) are lifted `11px` above their bar in the PSX font, but only `7px`
under the HQ font — the HQ glyphs are drawn a few pixels above the nominal `y`, so
the PSX-tuned lift would leave a visible gap.
