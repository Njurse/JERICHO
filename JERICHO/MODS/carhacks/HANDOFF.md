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
| The level's palette rows for a guest are found by the set the car actually reads | alias/row lines; `CarPalIndexForBuild` agreement |

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
   the final placement (the pin asks the pool first at draw time). `CAR_PIN_MAX` is now 24
   (3 guest cities × up to 8 sets), and the guest-set `row -1` CLUT refusal is fixed (re-points
   to the block base row). Still open on this item: the set-0 polys (~16-114 per car sample
   the host's shared page) and a possible collision with **region-streamed** textures
   (`HostUsesTPage` checks `permlist`, not the spool's dynamic building pages) — the one path
   that could still corrupt scenery, unverified.
4. **Assert leave/drop on the survivors.** The roster-removal fix has no test, which is why it
   kept recurring. Cover a clean leave (`MP_TEST_LEAVE`) and a hard drop, each then rejoining.
5. **Host only when there is a match.** The listener outlives the session: `MpHostEnd` is
   called only from `MpLeaveSession`, and `MpSessionReset` never closes it. Advertising is
   already gated on `MpStartMatch`; the listener is not. (A sticky listener already cost a
   real "i couldnt reconnect" bug, fixed by making a join a handover.)
6. **Live-join / hotload for a guest model is engine-side work (deferred, tracked).** A client
   joining a match already running cannot hotload a guest city's car: `JerHotLoadCarModel` /
   `JerHotLoadCarTpages` cover a pick that changes mid-match, but a *joiner's own* guest city
   needs the engine's `malloctab` region and the `MP_ADAPTER.md` notes. Lobby joins (the
   common case) are fine; this is the "a fifth friend joins mid-race" case, and it is
   deliberately out of scope until the lobby case is solid.

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
