# Caine's Crossfire tools

Everything in this folder is for driving and inspecting the game from a terminal. All
of it is source-of-truth here; copies for convenience also sit in
`src_rebuild/bin/Release_dev/`.

## Debug flags the engine understands

These only exist in `Release_dev` (they are inside the engine's `DEBUG_OPTIONS`
block). All default to off, so shipping behaviour is untouched.

| flag | what it does |
|---|---|
| `-frames N` | run N gameplay frames, then exit **cleanly** (that is the point: `exit(0)` runs `atexit(PsyX_Shutdown)`, which finalises the log — a kill throws the log away) |
| `-seed N` | pin every module's run randomness, so two runs are comparable and an A/B diff means something |
| `-level <city>` `-car slotN` `-mp 0\|1` `-weather <w>` `-time <t>` `-gamemode <g>` | the pre-existing boot options |

Every `-frames` run ends with one parseable line, which is what harnesses should read
instead of guessing at prose:

```
JERICHO-RUN: level=HAVANA car=1 frames=300 seed=7 status=ok
```

A run that crashes or hangs never prints it, so **its absence plus a timeout is the
failure signal**. Note `-frames` counts gameplay frames; the same tick is called from
the frontend state as well, but a frontend run does not load in every environment.

## Scripts

| script | what it does |
|---|---|
| `devcheck.sh [frames]` | build, run the cross-city scenario matrix, print one verdict. Exit 0 = all clean. Restores your `carhacks.ini` afterwards |
| `arena_test.sh [frames]` | one random city/car/weather/time arena run. `SEED=N` replays an exact scenario, because the seed picks the scenario too, not just module randomness |
| `levpages.py <city.LEV>` | read a level file's citylumps and segment sizes without launching the game |
| `launch_*.bat` | boot a specific scenario for playing. `test [frames]` makes it self-terminate and print a replayable seed; `dry` prints the roll without launching or writing config |

## House rules these follow, learned the hard way

- **Never kill by image name.** `taskkill /IM REDRIVER2_dev.exe` also kills the user's
  own session. Harnesses here let the game exit itself; the only kill is PID-scoped
  and only fires on a genuine hang.
- **Never delete `REDRIVER2.log`** — the user's sessions write it too. Snapshot it.
- **A launcher's `start` detaches**, so its PID is unknowable afterwards: never kill
  after a launcher run. That is why the test modes self-terminate instead.
- **Confirm the build before trusting behaviour.** A failed build leaves the previous
  exe on disk, which silently ignores any flag it predates — this has produced
  "the flag did nothing" more than once. Check the link line or the exe timestamp.
- **`REDRIVER2.log` is truncated at session start and flushed at close.** A kill
  discards the whole session, and reading a stale line-count boundary reads nothing.
- The engine's own `printInfo` lines are the evidence: `JERICHO-RUN:` for a verdict,
  `cross-city:` for imports, and `JERICHO-CHECK`-style invariants where they exist.
