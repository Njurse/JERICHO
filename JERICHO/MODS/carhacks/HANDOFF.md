# Cross-city / mp handoff — where this workstream stands

**Read this first if you are picking up the carhacks + mp cross-city work.** It exists so a
fresh session does not have to re-derive a long chain of measurements. The per-topic details
live in the topic docs (`CROSS_CITY.md`, `VRAM.md`, `PALETTES.md`, `mp/docs/ARCHITECTURE.md`,
`MP_ADAPTER.md`); this is the *state*.

## The goal

A player in a multiplayer match can pick a car from **any** city, and every machine (host and
all clients) draws that exact car — right model, right textures, right colours — whether the
pick happens before the match or mid-match, and whether the chosen car belongs to the level's
own city or a guest city. Secondarily: cars must clean up correctly when a player leaves, and
the mp session must not misbehave around hosting/joining.

## The 2026-10 pass (what changed, and what is left)

Four defects, all "the resource is not what it seems". Each is committed and verified; the
detail lives in the topic docs.

1. **A car page numbered `set 0` was never paged in** (`d1cars` bake). `texture_set 0` is the
   engine's "no page" sentinel - the pin walk skips it and `CarSetRemap` sends it to a HOST
   slot - and `build_page_plan` handed the FIRST used page that number. Measured on the baked
   NEWCASTLE blob: model 1 = 75 of 135 polys on set 0, model 2 = 78 of 139, model 3 = 20 of 233
   (only its untextured ones) - the "invisible hood on car 1, cars 1 and 2 half broken, car 3
   fine" report. The bake now numbers from 1 (`PAGE_SET_BASE`) and drops Driver 1's flat
   untextured polys instead of faking them as textured on set 0. See `d1cars/docs/DRIVER1.md`.

2. **A palette row the lump cannot fill was FABRICATED** (`cars.c`). The alias copied a
   neighbouring row's six columns including slot 0 - which the draw then accepted as this
   page's paint. Now the row is left empty and its polys draw their own page CLUT. See
   `PALETTES.md` §7, which also records the still-open "extra panels" corruption.

3. **A mid-match hot load exhausted the level's CAR_POLY arena** (`cars.c`, `cars.h`,
   `models.c`) - the real "cars go invisible after cycling": the bump cursor reached
   `MAX_CAR_POLYS` and every later model built 0 polys, invisible while its pages still
   uploaded. A hot load now takes a block of its own arena and `JerReleaseCarGeometry` returns
   it. Measured before/after with `JERICHO_DIAG_CARDRAW=1` and the new "produced 0 polys" line.

4. **A car change while ON FOOT left the player's Tanner standing** (`mp`). `MpChangeCar` used
   `ChangePedPlayerToCar` alone; the engine's own `PedGetInCar` also destroys the ped and gives
   its Tanner slot back. Added `RemovePlayerPedestrian`.

**State:** `chk_suite.sh` reports "all 7 rows clean" (the 3-city mix row used to read FAILURES
on a clean build - a `crosscheck.py` INV3 bug, now fixed so each set is judged against its own
source city). A 12-switch multi-city cycle builds every car, plateaus its pool and returns
everything. **Open:** the extra-panels corruption above.

**Slot reuse (later, 2026-10).** A car change now reuses the slot the old car vacated, so a
change holds ONE slot instead of the transient TWO that made concurrent changes run the import
pool dry. This is **local-only** (mplive.c, `ebcb5792` + `fec7def4`): the fold that re-models a
*remote* car's slot cannot free it directly, because the remote car is still drawn on it — the
`slotrelease.h` "cars on it → deferred" rule — so the peer/fold side still takes a canonical
spare. The agreed set (`CHK_NET_SET`) is the slot-authoritative mechanism for the peer side;
making the fold apply it mid-match *regressed* (273 refusals vs 17) because the fold-side
re-model needs the deferred ordering first, so that half was reverted. Measured: `mp_carstress`
3 seats / 60 s = 114 REUSED / 27 fresh, 21 refusals, coverage 41/41/41, no dumps.

## The 2026-10-11 palette pass (make it provable first, then fix it)

The palette work began by making the failure **provable**, because the acceptance suite was
green while rows a built model reads had no palette at all.

* **The gate can fail now.** `crosscheck.py` **INV2** (a guest set resolving to a HOST
  `civ_clut` row) is a hard FAIL, and **INV4** is new: the rows a built model READS must
  carry data, no `civ_clut` row may be written by two cities, no model may bake sets with no
  palette row of their own, 0 pins DROPPED / palette rows REFUSED, and the lower half pool
  must stay inside its limits. `vrammap.py`'s CLUT-column check reads the lower-half column
  (`y >= 512`) instead of the dead 480..511 band, and `chk_mashup.sh` dumps VRAM and runs
  crosscheck too.
* **The rig stopped minimising.** Harnesses run the game in the normal shipped window
  (windowed 1280x720); `run_minimized.ps1` is gone. A minimised or fullscreen window renders
  at a degenerate size, so a VRAM dump or any capture stops being representative.
  `chk_suite.sh` forces `config.ini` windowed and restores it — with `carhacks.ini` and the
  modlist — on any exit.
* **Carhacks' player-facing lines go to the unified console.** The module's eight
  `jer_error` toasts are now `jer_console_log`, so a refused pick or a failed hot-load reads
  in the same bottom-left feed mp uses; `jer_error` is no longer used in the module. (The
  console's *status* lines are hidden while the console is toggled off.)

**Measured, and it changes what "fixed" means.** An empty palette row is a defect only when
the SET has data the row mapping failed to place. CHICAGO's `LUMP_PALLET` carries palettes
for page sets **{1, 50, 62, 63, 65}** only, so its sets 58, 54 and 55 have no palette data
anywhere — rows base+1/+6/+7 are correct on slot 0, and INV4 reports them as warnings. The
real defect is a set with no row of its own collapsing onto the block base row (today:
HAVANA 21 → row 8, VEGAS 68/67/4 → row 16, RIO 69/70/1 → row 24 — "those panels wear each
other's colours"). See `PALETTES.md` §7.

**Open:**

- the per-model row assignment for that collapse (rows from `CarModelSet`/`CarModelSetCount`
  at bake time — the real fix, since 8 rows cannot hold 13 sets), plus arbitration so two
  cars never share a row;
- **DONE (2026-10-11): hide D1 cities (MIAMI/FRISCO/LA/NEWYORK/NEWCASTLE) when their
  Driver 1 assets are absent.** The list is generated (`d1citymodels.h`) rather than read,
  because the frontend builds it before the CD subsystem is up, so it used to offer them
  unconditionally — a plain Release install showed five cities whose every car failed to
  build ("in release the palettes on some vehicles from d1 are busted" was this, not a
  palette bug). One gate now covers all three surfaces: `chkCityAssetsPresent()`
  (`carimport.c`) tests `DRIVER\D1CARS\<CITY>.LEV` with a RAW CWD-relative `fopen` — not
  `FileExists()`, which resolves against `gDataFolder` and would probe
  `DRIVER2\DRIVER\D1CARS\...` (the trap `jer_d1_file_exists` hit) — and
  `chkFillCarTableFromImport`, `mplive.c`'s `ChkMpCities` and carselect's city row all
  gate on it. d1cars is now part of the release too (see below), so a shipped copy has
  the data and shows the cities;
- the D1 exporter's own freshness is a **separate workstream** (out of scope here).

## The in-game picker's city row (fixed)

"The pause menu's `Vehicle Options` picker only shows the host city, so I cannot reach
another city's cars in game" had one cause: `ChkMpCities` (carhacks/mplive.c) answered
`MP_CARQ_CITIES` only when the `cross_city_vehicles` config key was on, and that key
defaulted to off — so mp was handed `count = 0` and offered the session's own city alone,
while the frontend car screen (which has no such gate, because a PICK is what imports the
car) still browsed every city. `ChkMpLoad` carried the same gate, so a mid-match pick from
another city was refused in silence even once the row offered it.

Now:

- both the city answer and the slot answer are owned by **`unlock_extra_vehicles`** — the
  hack that fills the frontend roster — so the two car screens cannot disagree;
- `MP_CARQ_LOAD` has **no config gate**: a player's explicit pick imports the car in game
  exactly as it already did from the frontend menu;
- **`cross_city_vehicles` defaults to on**, and the rigs that restore a caller's
  `carhacks.ini` no longer write `cross_city_vehicles = 0` when there was no saved file
  (that silently undid the new default).

Measured (Release_dev): a CHICAGO pair with the pause opened logs, on both seats, `9
city/cities on the row - CHICAGO HAVANA VEGAS RIO MIAMI FRISCO LA NEWYORK NEWCASTLE`, and
`MP_TEST_CITYCHANGE` reaches HAVANA. Single player with `cross_city_vehicles = 0`: the
picker still offers HAVANA's 9-slot roster and the pick still imports (`car data from
HAVANA read MID-LEVEL`). The picker's own line (`[mp] car select: N city/cities on the
row - ...`) is the instrument: it is what a padless run reads.

**Observation, not fixed (not reproduced, cause not established).** In two of five headless
single-player runs the engine pause opened by itself (an external START; the pause hook
does not fire in the other three), and once open the picker's city row showed the LAST city
with a car change applied — i.e. the pause was steered with no pad. The handlers involved
(`MpCcAdjustCity`, `MpCcApply`) are `mp.c`'s picker rows and are untouched by the change
above; they are only reachable from engine pause input. Worth an investigation if a player
ever reports "my car changed by itself when I opened the pause".

## The rig (use it before you touch the game)

```
# one command, the shape the user tests by hand: a host on its own city, a client joining
# with a car from another city, per try
python JERICHO/MODS/mp/tools/mp_tries.py --keep --seconds 55

# a single try, with an assertion that a line MUST appear
python JERICHO/MODS/mp/tools/mp_tries.py --try havana:12 --require "draws exactly that"

# the car-switch release scenarios (#14/#12): T2 switch until spares exhaust, T3 a shared
# car survives a leave, T5 a deferred release, T6 an import shared by two players
python JERICHO/MODS/mp/tools/mp_tries.py --scenario all --keep

# two real instances on one PC (also 3..8 seats), the underlying harness
MP_TEST_FRONTEND_JOIN=1 CHK_FORCE_MENU=1 python JERICHO/MODS/mp/tools/mp_localpair.py \
    --players 3 --bot chase --keep
```

- `mp_tries.py` prints a per-try table plus **both seats'** identity and page evidence. A try
  FAILS on an identity problem even when the harness verdict says PASS — "correct on the host
  but the client was still the old car" is invisible to a verdict.
- `--require [SEAT=]REGEX` asserts a line must appear; `--forbid` asserts one must not. SEAT is
  `host`, `client` (any joiner) or `clientN` (the Nth joiner).
- Known mid-match disconnect is classified (`DROPPED`), not reported as a stall.
- `--keep` copies each try's two logs to `.mp-tries/tryN/` — read them there, not from the
  shared `JERICHO.log` (see traps).

## Verified working now

| thing | how it is proved |
|---|---|
| A player's identity is the car they **chose**, not the seat the engine put them in | `chkImportChosenCar` + `chkNetLocalCar`; advert stays the chosen car across a re-seat |
| A guest car is hot-loaded mid-match: geometry, cosmetics, per-slot source, texture pages + rows | `JerHotLoadCarModel` / `JerHotLoadCarCosmetics` / `JerHotLoadCarTpages`; host log "draws exactly that (slot 7) - their own colours" |
| Leaving releases a slot; leaving the session releases everything | `JerReleaseCarSlot` / `JerReleaseAllCrossCity` / `chkImportReleaseAll`; the leaver's log gives back every slot and the buffers. Since #12/#14 a peer's leave or car change goes through the release routine (`chkNetReleaseSlotIfUnused`: kept while another player names the car, deferred while a car is on the slot). Run on Windows 2026-10-04 (build `0.9.0-42-g1d88177d`): the three verdicts appear as documented (`released - nobody names it and no car is on it`, `kept - still named by player N`, `deferred - car N still on it`) and T2's pool summary stays flat across seven switches. See "Releasing a slot - measured (Windows)" in MP_ADAPTER.md |
| A departed player's car disappears from the other machines | roster removal in `MpHandleRoster` (`MpRemovePlayer`) |
| A join retries three times before failing | verified against a dead port: "attempt 1 of 3 ... retrying" x2, toast only after the third |
| A refused car load heals by itself | the fold retries a busy slot / unbuilt car every ~0.4 s and gives up once at ~8 s; `CHK_REFUSE_LOAD=3` on a host: 3 refusals, then `-> resident slot 7` and `draws exactly that (slot 7)`. See MP_ADAPTER.md "Retrying a load that could not happen yet" |
| A session ends cleanly on the way to the frontend | `MpSessionReset` closes the listener + discovery (so an ended host stops accepting peers) and forgets a pending "Change car" |
| The level's palette rows for a guest are found by the set the car actually reads | alias/row lines; `CarPalIndexForBuild` agreement |
| The in-game `Vehicle Options` picker offers **every** city, and a mid-match pick imports with `cross_city_vehicles` off | `[mp] car select: 9 city/cities on the row - CHICAGO ... NEWCASTLE` on both pair seats; `MP_TEST_CITYCHANGE -> HAVANA model 1` -> `[carhacks] import: now driving HAVANA model 1`; single player with the key off: `PAUSECAR city 1 roster: 9 slot(s)` then `car data from HAVANA read MID-LEVEL`. See "The in-game picker's city row (fixed)" above |

Commits: `f3d65d36`, `29118821`, `103bf4d5`, `917fe509`, `248c6909` (all on `main`, pushed).

## Open, in the order worth doing

1. ~~The VEGAS palette walk consumes its lump ~2.6x too fast.~~ **Resolved 2026-10 — a
   misdiagnosis; the walk was always correct.** The "200 entries read" in the old
   `no terminator within 14808 bytes` line was `clutStored` (the **inline-CLUT** count), not
   the record count: VEGAS reads all 525 records — 200 inline CLUTs + 325 "reuse an earlier
   CLUT", and the summary line `325 reusing an earlier CLUT` proves it. The lump ends in a
   **lone 4-byte `-1`** (not a full 16-byte record), and the old guard required 12 bytes of
   headroom *before* the terminator test, so it tripped 4 bytes early on every city and
   printed a false "the lump is mis-sized". Fixed in `Game/C/cars.c`: the `-1` terminator is
   read first (4-byte guard), then the full 16-byte record is required; the false message is
   gone and the walk ends on the real terminator.
2. **Switching cities in the vehicle selector disconnects the client.** Reported with a
   *git-release host against a develop client* — **rule the build mismatch out first** by
   reproducing on matched builds in the rig. The module's city row itself is bounds-checked
   (`CarAvailability[city][slot]` / `carNumLookup[city][slot]`), so suspect what a city change
   *re-draws* (the icon reads another city's car data while a menu is up). Add a city-switch
   step to the harness (step the row; don't just set it at start). If it faults, take the dump
   and attribute it with `tools/dmp_fault.py` + `tools/map_lookup.py` **before** touching code.
3. **Imports take the lower-half pool (rows 512..1023) — in place, and the remaining gaps are
   narrower than they looked.** Measured 2026-10: every SP mashup (0..3 guest cities, up to
   11 cars) pins into the pool with **0 world pages evicted** and **0 pins dropped** — the old
   "evicting the world if needed" and `0 in the pool` lines were *load-time snapshots*, not
   the final placement (the pin asks the pool first at draw time). `CAR_PIN_MAX` is now 30
   (the lower-half pool's own size; a 3-guest-city mashup asks for 13 pages), and the guest-set `row -1` CLUT refusal is fixed (re-points
   to the block base row). Still open on this item: the set-0 polys (~16-114 per car sample
   the host's shared page) and a possible collision with **region-streamed** textures
   (`HostUsesTPage` checks `permlist`, not the spool's dynamic building pages) — the one path
   that could still corrupt scenery, unverified.
4. **Assert leave/drop on the survivors.** The roster-removal fix has no test, which is why it
   kept recurring. Cover a clean leave (`MP_TEST_LEAVE`) and a hard drop, each then rejoining.
5. ~~**Host only when there is a match.** The listener outlives the session: `MpHostEnd` is
   called only from `MpLeaveSession`, and `MpSessionReset` never closes it.~~ **FIXED
   2026-10.** `MpSessionReset` (the one primitive every "session over" path goes through -
   `MpReturnToFrontend` and both `Begin*`) now closes the listener and stops discovery when
   one is up, so a host that ends its match any other way no longer comes back to the
   frontend still accepting peers into a session that is gone (and no longer holds the
   port). A pending "Change car" is forgotten there too - add `MpHostEnd`/`MpDiscoveryStop`
   to `MpSessionReset` (mp_session.c). See the note on `MpSessionReset`.
6. **Live-join / hotload for a guest model is engine-side work (deferred, tracked).** A client
   joining a match already running cannot hotload a guest city's car: `JerHotLoadCarModel` /
   `JerHotLoadCarTpages` cover a pick that changes mid-match, but a *joiner's own* guest city
   needs the engine's `malloctab` region and the `MP_ADAPTER.md` notes. Lobby joins (the
   common case) are fine; this is the "a fifth friend joins mid-race" case, and it is
   deliberately out of scope until the lobby case is solid.
7. **The VEGAS pickup (model 4) "stretched geometry" — investigated 2026-10, NOT
   reproduced.** The report is a few triangles stretching to extreme distances on an
   ambient AI car. Ruled out by measurement: the source model is clean (129 verts, max
   vertex index 128; those counts are right), the denting buffers are bounded
   (`MAX_DENTING_VERTS` 256 > 129), the drawn body vertices match the clean model (the
   `STRETCHED VERTEX` canary, `cars.c`), and the baked polys index the model's own vertices
   correctly (the `STRETCHED POLY` canary added alongside it). Booting VEGAS with the player
   in model 4 for 900/1800/2500 frames — player and ambient, detail and low detail — trips
   neither. So it is NOT the body vertices, NOT the poly indices, NOT the denting copy
   bound, and NOT a crumple displacement (see below). **The instruments are in the build:
   a live occurrence will print `[cars] STRETCHED VERTEX` / `STRETCHED POLY` with the car,
   slot, vertex/poly and values — ask for that line.** One lead is on file: `NewCarModel[].
   vlist` is a SHARED template field that `DrawCar` repoints at the last-drawn car's dump
   EVERY frame, so any consumer of it outside `DrawCar` reads a different car's vertices.
8. **Body dents were ~4096x too weak (FIXED 2026-10).** `crumpleApplyImpact`'s body path
   divided by `impactNormalize` without the `<< 12` the wheel path has, so `strength` landed
   in 0..1 and every downstream `FIXEDH()` made it ~0 — a crash left the car undamaged. Now
   it matches the wheel path. This is the OPPOSITE of the stretch report, so it was not the
   cause, but it is real and it is fixed.

## Traps that cost real time here — do not re-learn these

- **`wantedCar[]` holds a MODEL, not a resident slot.** `mission.c` compares it against the
  model each resident slot carries; `players.c` derives the slot from the model and already
  prefers an **imported** slot. Writing a slot index there made a guest resolve to nothing.
- **"player 0" is not the picker.** Which number means the local player depends on the seat,
  and the number after `slot` in the mp lines is a resident slot, not a car slot. Use the
  `[local= isLocal localId= row=]` field these lines now carry.
- **A module fix may only free or claim state the module created.** Two attempts to fix
  textures by reaching into the engine's page/CLUT space broke the *frontend* (menus stopped
  updating while input worked). The frontend's index space is not ours.
- **`mp_localpair.py`'s default game dir is CWD-relative**, so run it from the repo root or
  pass an absolute `--game-dir`; otherwise it fails before launching anything.
- **Both instances write the same `JERICHO.log`**, and each *start* truncates it. Use `.mp-pair`
  seat logs / `--keep` copies, and never delete the shared log or the crash dumps (the user's
  own sessions use them).
- **Never kill by image name.** PID-scoped only (`kill_shell` on your own job). The user's
  interactive session runs the same exe.

## House rules for this repo

- Small, working, incremental commits as the work happens; engine change first, then module,
  then docs. Verify before each.
- Keep green: `python tools/doccheck.py`, `python JERICHO/MODS/mp/tools/check_debug_independence.py`,
  `JERICHO/MODS/carhacks/tools/chk_suite.sh` (its `mix 3/3` crosscheck failure is pre-existing).
- Build: `cd src_rebuild && cmd //c build_dev.bat`, run dir `src_rebuild/bin/Release_dev`.
- A live host for hand testing: `MP_DEBUG=1 MP_AUTOSTART=host ./JERICHO_dev.exe -nointro -nofmv -host 1318`
  (it self-terminates if you pass `-frames N`; otherwise kill it by PID).
- The user's two-PC run is the acceptance test; headless runs cannot judge how colours *look*,
  only the palette/page state behind them.
