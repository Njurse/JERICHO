# CARSELECT.md — the carhacks car-select menu

A JERICHO frontend menu (`carselect.c`) that **replaces the stock "Choose A Ride"
car-select screen** for a single-player Take a Ride, so the car list can carry one
row the stock screen has no room for: **which city's roster it is drawn from**.

```
    < CAR >        left/right cycles the car inside the roster below
    < CITY: X >    left/right cycles CHICAGO/HAVANA/VEGAS/RIO - the roster  (NEW)
    Ride           start the level with the picked car
    Back           back to whatever opened the car screen (Day/Night)
    Triangle       the same back, from any row
```

The menu is shown by **replacing** the stock car screen on the nav stack
(`jer_frontend_open_replace`, `jer_frontend.h`) - it sits in that screen's slot, not on
top of it. So Back (the Back row, an `is_back` row the engine drives) and Triangle (the
engine's own back) are the stock car screen's *own* previous-screen pop: they return to
whatever opened the car screen. The menu defines no back of its own - the old
`chkSelBack` is gone, because a push would have left the car screen on the back stack and
trapped the player.

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
after it *starts* its setup (`FEmain.c`). **Outside a session** it declines unless
**both** of:

| condition | why |
|---|---|
| `GameType == GAME_TAKEADRIVE` | a mission's own car pick keeps the stock screen |
| `NumPlayers == 1` | 2-player split-screen keeps the stock screen |

**Inside a session** those two say nothing and are not asked: the LAN chain leaves
`NumPlayers` at 2 and a joiner is still `GAME_MISSION` until mp launches, so the
old condition bailed every time — silently, with no car-select line in the whole
session — which is why a joining player never saw the roster. With a session live
the menu is offered too, and `Ride` hands the launch over instead of starting the
level:

* `Ride` fires `JER_EVENT_MP_FRONTEND` with `JER_MP_FE_START` — the same event the
  stock frontend raises on `BTN_START_GAME` — so **mp** launches
  (`MpClientLaunch` for a client, `MpStartMatch` for the host) and picks the pick
  up from `wantedCar[0]` on the way. Starting the level from here would load it
  without the session's level, car agreement and spawn.
* If the session does not take the launch (a refused join), the menu says so and
  falls through to `SetState(STATE_GAMESTART)`, so a dead `Ride` cannot happen.
* `Back`/Triangle in a session are the same engine pop - it returns to mp's own
  chain, the screen that opened the car screen.

The menu opens on the next frontend frame, not from the hook: `CarAvailability` is
only final *after* the setup that hook interrupts — reading it any earlier hands
back the previous level's list (the open log prints the count, e.g. `10 car(s) in
its roster`, which is how you can tell the setup landed).

`Back` goes to whatever opened the car screen. In the stock Take-a-Ride chain (main 0
→ city 1 → day/night 3 → car 14) that is the **Day/Night screen** (index 3). Because
the menu took the car screen's slot, the Day/Night screen's own Back then carries on to
the city screen - the loop the old push created (popping to the pushed car screen,
re-running its setup and re-arming this menu) is gone.

## The cross-city consequence

Picking a car from **another** city's roster is a real cross-city import: the pick
becomes an entry in the level's import set (`carimport.c`), into a spare resident
slot, and the engine reads that model from that city's files. `InitPlayer` prefers
a slot the model was *imported* into over a native one with the same number
(`players.c`), so the foreign car wins even when the level also lists that number.

Picking from the level's own city needs no import **when the level's pool already
holds that model** — a level reads only the models its own list names, so an
own-city model outside that pool (Rio's model 12, the special, is in Rio's files
and in no Rio take-a-ride level) is imported from its own city's files exactly
like a guest. Without that the engine had nothing to build the car from and fell
back to resident slot 0 — the level's *first* car — which is what "I picked car 12
and spawned as car 1" was.

The engine reads a level's car data from **as many** cities as the set names
(`models.c` keeps a source city per resident slot), so a mixed set builds and
spawns; each city needs a palette block, and the bank holds three
(`civ_clut` rows 8..31). See `CROSS_CITY.md` for the mechanism and `MP_ADAPTER.md`
for what that means in a session.

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
