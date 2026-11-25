# City Cycle (`citycycle`)

Stress test for cross-city cars and the import page-handle allocator.

Set `citycycle = 1` in `JERICHO/CONFIG/modlist.ini` (needs `carhacks`), start any level, and
stay put. After a short settle it keeps four cars in front of you. Every ~7 s it removes them
and builds four new ones from **1, then 2, then 3, then 4 different cities** (repeating), with
the cities and models picked at random, so you see one city's cars, then two cities', and so on,
while carhacks loads and releases the cities underneath.

Each round it prints, to the log and the in-game console:

```
[citycycle] round 6: 4/4 cars up, 4 drawable
```

`<-- CHECK THE LOG` is appended when a car did not spawn or is not drawable; the log lines
above it name the car, its city and why. After every round the `JERICHO-INDEX:` census is
printed (handles held / free / leaked) - leaked should always be 0, and held should fall back
once a city's cars are gone.

`JERICHO_CYCLE_SEED=<n>` repeats a run. Constants (cars, interval, start frame) are at the
top of `citycycle.c`.

What to look for: `no free page number`, `NOT drawable`, `JERICHO-CLUT COLLISION`, leaked
handles, wrong textures or wheels on a car, and the frontend/menus after quitting the level.
