# CARSELECT.md — the carhacks car-select menu

A JERICHO frontend menu (`carselect.c`) that **replaces the stock "Choose A Ride"
car-select screen** for a single-player Take a Ride, so the car list can carry one
row the stock screen has no room for: **which city's roster it is drawn from**.

```
    < CAR >        left/right cycles the car inside the roster below
    < CITY: X >    left/right cycles CHICAGO/HAVANA/VEGAS/RIO - the roster  (NEW)
    Ride           start the level with the picked car
    Back           back to the Day/Night screen
    Triangle       the same back, from any row
```

Triangle is the back button on every stock frontend screen, so the menu is offered it
too (`JER_FE_MENU.on_back`, `jer_frontend.h`). It performs the Back row's action from
wherever the cursor is, so a player who reaches for Triangle - as they would on any
stock screen - is not stuck. The two share one function (`chkSelBack`) so they cannot
drift apart.

It is a **pad-only** path: a module menu ignores input when no pad is connected, and a
headless run has no pad, so no harness lever can drive it (see "Driving it without a
pad" below - those env vars drive the *state*, not the buttons).

The roster row sits **directly below** the car row. The stock screen can only ever
show the level's own list (`carNumLookup[GameLevel]`, `FEmain.c`); this one browses
any of the four cities'.

## Why a menu instead of editing the screen

The stock screen is engine code with three buttons (Previous/Select/Next) and no
row list, so a fourth option would be surgery on the frontend. A module-provided
JERICHO menu gives the rows for free, keeps the change carhacks-side, and — the
point — leaves the engine's **asset loading completely alone**:

* the stock screen still runs its own setup, which is what computes
  `CarAvailability` and loads the icon background/palette (`DATA\CARS\CARBACK.RAW`);
* the menu is opened **one frame later**, in its place;
* `Ride` makes the same call the stock `Select` makes — `wantedCar[0] = model`
  then `SetState(STATE_GAMESTART)` — so the level loads exactly as before and only
  the *choice* is ours.

## What the roster is

The city's **own** frontend list: slots `0..9` where `CarAvailability[city][slot]`
is non-zero, with the model number from `carNumLookup[city][slot]`. So a foreign
city shows its own availability, not the level's. The icon is the engine's own
(`JER_FE_MENU.get_preview` returns the car's **own** city + model), so a Havana car
previews with Havana's art while the level is Chicago.

Consequence worth knowing: a city with no data for a model simply does not list it
(Chicago's model 11 has no `CARMODEL` and `CarSelectScreen`'s setup already
excludes it — see `VEHICLES.md`).

## When it is used, and when it is not

Armed from the `JER_EVENT_CAR_AVAILABILITY` query, which the stock screen fires
after it *starts* its setup (`FEmain.c`). It declines unless **all** of:

| condition | why |
|---|---|
| `GameType == GAME_TAKEADRIVE` | a mission's own car pick keeps the stock screen |
| `NumPlayers == 1` | 2-player split-screen keeps the stock screen |
| no live mp session (`jer_net_is_active()`) | that screen is where **mp** seats players and claims the START (`JER_EVENT_MP_FRONTEND`), so overriding it from another module would break the match |

The menu opens on the next frontend frame, not from the hook: `CarAvailability` is
only final *after* the setup that hook interrupts — reading it any earlier hands
back the previous level's list (the open log prints the count, e.g. `10 car(s) in
its roster`, which is how you can tell the setup landed).

`Back` goes to the **Day/Night screen** (index 3 — the stock Take-a-Ride chain is
main 0 → city 1 → day/night 3 → car 14), *not* to the stack. Returning to the
stock car screen would re-run its setup, re-arm this menu and trap the player.

## The cross-city consequence

Picking a car from **another** city's roster is a real cross-city import: the pick
becomes an entry in the level's import set (`carimport.c`), into a spare resident
slot, and the engine reads that model from that city's files. `InitPlayer` prefers
a slot the model was *imported* into over a native one with the same number
(`players.c`), so the foreign car wins even when the level also lists that number.

Picking from the level's own city needs no import at all (the level already lists
it) and says so in the log.

The engine reads a level's car data from **one** foreign city (`models.c`), which
is why a second one is refused loudly rather than silently resolving in the first
city's table. See `CROSS_CITY.md` for the mechanism and `MP_ADAPTER.md` for what
that means in a session.

## Config

| key | default | meaning |
|---|---|---|
| `car_select_menu` | 1 | offer the menu at all (`carhacks.ini`, section `carhacks`). Off ⇒ the stock car screen, untouched. |

## Driving it without a pad

A module menu ignores input when no pad is connected, and a headless run cannot
navigate the frontend either, so the harness walks and picks for you:

| env | meaning |
|---|---|
| `CHK_FORCE_MENU=1` | walk to the stock car screen (sets `GameType`/`NumPlayers`/`GameLevel`) so the intercept fires |
| `CHK_FORCE_ROSTER_CITY=0..3` | pick this city's roster |
| `CHK_FORCE_CAR=n` | pick the car at 0-based index `n` in that roster |
| `CHK_FORCE_LEVEL=0..3` | the level to start (default 0) |

Measured (Release_dev, `-nointro`):

```
# menu + intercept, level RIO, browse CHICAGO's roster, car 5 (model 8)
[carhacks] car select: harness - walking to the car screen (level RIO)
[carhacks] car select: armed for level RIO (its car screen is showing)
[carhacks] car select: opening the menu over the stock car screen (level RIO, 10 car(s) in its roster)
[carhacks] import: pick set to CHICAGO model 8
[carhacks] car select: RIDE CHICAGO slot 5 -> model 8 (level city RIO) wantedCar=8
[carhacks] import: the pick (CHICAGO model 8) -> resident slot 5 (level RIO)
[carhacks] import: slot 5 <- model 8 from CHICAGO
cross-city: slot 5 geometry from CHICAGO model 8
JERICHO-RUN: level=RIO carslot=5 model=8 frames=300 seed=0 status=ok
```

and a pick from the level's own city, which imports nothing:

```
[carhacks] car select: RIDE RIO slot 1 -> model 2 (level city RIO) wantedCar=2
[carhacks] import: the pick (RIO model 2) is this level's own car - no import
[carhacks] import set: level RIO, guest city level, 0 entries, version 1
JERICHO-RUN: level=RIO carslot=1 model=2 frames=200 seed=0 status=ok
```
