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

## The rig (use it before you touch the game)

```
# one command, the shape the user tests by hand: a host on its own city, a client joining
# with a car from another city, per try
python JERICHO/MODS/mp/tools/mp_tries.py --keep --seconds 55

# a single try, with an assertion that a line MUST appear
python JERICHO/MODS/mp/tools/mp_tries.py --try havana:12 --require "draws exactly that"

# two real instances on one PC (also 3..8 seats), the underlying harness
MP_TEST_FRONTEND_JOIN=1 CHK_FORCE_MENU=1 python JERICHO/MODS/mp/tools/mp_localpair.py \
    --players 3 --bot chase --keep
```

- `mp_tries.py` prints a per-try table plus **both seats'** identity and page evidence. A try
  FAILS on an identity problem even when the harness verdict says PASS — "correct on the host
  but the client was still the old car" is invisible to a verdict.
- `--require [SEAT=]REGEX` asserts a line must appear; `--forbid` asserts one must not.
- Known mid-match disconnect is classified (`DROPPED`), not reported as a stall.
- `--keep` copies each try's two logs to `.mp-tries/tryN/` — read them there, not from the
  shared `JERICHO.log` (see traps).

## Verified working now

| thing | how it is proved |
|---|---|
| A player's identity is the car they **chose**, not the seat the engine put them in | `chkImportChosenCar` + `chkNetLocalCar`; advert stays the chosen car across a re-seat |
| A guest car is hot-loaded mid-match: geometry, cosmetics, per-slot source, texture pages + rows | `JerHotLoadCarModel` / `JerHotLoadCarCosmetics` / `JerHotLoadCarTpages`; host log "draws exactly that (slot 7) - their own colours" |
| Leaving releases a slot; leaving the session releases everything | `JerReleaseCarSlot` / `JerReleaseAllCrossCity` / `chkImportReleaseAll`; the leaver's log gives back every slot and the buffers |
| A departed player's car disappears from the other machines | roster removal in `MpHandleRoster` (`MpRemovePlayer`) |
| A join retries three times before failing | verified against a dead port: "attempt 1 of 3 ... retrying" x2, toast only after the third |
| The level's palette rows for a guest are found by the set the car actually reads | alias/row lines; `CarPalIndexForBuild` agreement |

Commits: `f3d65d36`, `29118821`, `103bf4d5`, `917fe509`, `248c6909` (all on `main`, pushed).

## Open, in the order worth doing

1. **The VEGAS palette walk consumes its lump ~2.6x too fast.** *Measured, and the size is
   NOT the problem* — see the "VEGAS palettes" memory or `levpalette.py` output: VEGAS's
   `LUMP_PALLET` segment is 14808 bytes and holds 525 records (~28 B each), yet the engine's
   walk in `Game/C/cars.c` (~2007-2160) stops after **200** entries. Its advances are
   `buffPtr += 4` (16-byte header) and `+= 8` (32-byte inline CLUT), all on `int*`, matching
   the tool — so the extra consumption is in a branch not yet read end to end.
   **Next action:** log `(char*)buffPtr - lump_ptr` once per entry, run one VEGAS guest try,
   and the branch shows itself. **Do not** widen the size or use `total_cluts` as a count;
   both are disproved.
2. **Switching cities in the vehicle selector disconnects the client.** Reported with a
   *git-release host against a develop client* — **rule the build mismatch out first** by
   reproducing on matched builds in the rig. The module's city row itself is bounds-checked
   (`CarAvailability[city][slot]` / `carNumLookup[city][slot]`), so suspect what a city change
   *re-draws* (the icon reads another city's car data while a menu is up). Add a city-switch
   step to the harness (step the row; don't just set it at start). If it faults, take the dump
   and attribute it with `tools/dmp_fault.py` + `tools/map_lookup.py` **before** touching code.
3. **Imports must take the lower-half pool (rows 512..1023) on SP *and* MP.** The pool is
   JERICHO's own space and identical in both; the streaming half is what differs per map.
   Today a set can land on a page the map streams into ("evicting the world if needed", and
   `slot N holds X: 2 pin(s) (0 in the pool)`), which is both the broken-texture and the
   scenery-contamination report — and it shows up on SP-side levels too, so it is not MP-only.
   Also account for a car's extra roof/window/special panels, each needing a paging slot, with
   long vehicles as the extreme case.
4. **Assert leave/drop on the survivors.** The roster-removal fix has no test, which is why it
   kept recurring. Cover a clean leave (`MP_TEST_LEAVE`) and a hard drop, each then rejoining.
5. **Host only when there is a match.** The listener outlives the session: `MpHostEnd` is
   called only from `MpLeaveSession`, and `MpSessionReset` never closes it. Advertising is
   already gated on `MpStartMatch`; the listener is not. (A sticky listener already cost a
   real "i couldnt reconnect" bug, fixed by making a join a handover.)

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
- A live host for hand testing: `MP_DEBUG=1 MP_AUTOSTART=host ./REDRIVER2_dev.exe -nointro -nofmv -host 1318`
  (it self-terminates if you pass `-frames N`; otherwise kill it by PID).
- The user's two-PC run is the acceptance test; headless runs cannot judge how colours *look*,
  only the palette/page state behind them.
