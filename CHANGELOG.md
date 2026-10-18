# JERICHO changelog

User-facing changes per release. Engine/module internals are documented in
[HOOKS.md](src_rebuild/Game/C/JERICHO/docs/HOOKS.md); the inherited REDRIVER2 history
is in [changelog.txt](changelog.txt).

## v0.10.4 (unreleased)

### Fixed
- **The player car no longer drives at "fast-forward" speed until another car comes
  near.** Since v0.10.3 the player car was moved twice per physics step whenever no
  other car was close (`GlobalTimeStep`, `handling.c`; regression from the MAX_CARS 32
  change), and went back to normal speed the moment traffic was alongside. It is moved
  once per step again, exactly as stock.
- **The sim runs at exactly 30 steps per second on every display.** The step gate now
  carries the remainder of each frame instead of dropping it, so a 144 Hz monitor no
  longer runs the game ~4% slow and players on different refresh rates stay in step.
  After a stall (loading, alt-tab) the game resumes at normal speed rather than
  rushing to catch up.
- **Multiplayer:** fast-forward (BACKSPACE, map screen) is locked for the whole
  session, including level load and the countdown, not only once the match is live.
- **Mod loader:** a `mod.toml` `id` can no longer point the loader at a library outside
  the module's own folder (partial fix for #4).

### Build and release
- Every downloaded build dependency (premake, SDL2, OpenAL, libjpeg) is fetched over
  https and verified against a pinned SHA-256, in CI and in both prepare scripts
  (partial fix for #8).
- A tagged release is published only when the docs/debug/mp checks pass; the rolling
  alpha stays tolerant.
- `jer_test` can now fail, and `build_test.bat` runs from any checkout (partial fix for
  #6). The prepare scripts point at `src_rebuild/build`, where premake actually writes
  (partial fix for #7).

### Diagnostics
- `JERICHO_HANDLING_PROBE=1`: per-second car lines gain `mbc=N/30` (steps with another
  car near); the rate line gains `resyncs=N` and a `BEHIND` marker when the sim runs
  under 28 steps/s. The boot log names the new gate.

### Known issues
- Open: #2, #3, #5, #11, #13, #17, #18, #19, #20, and the remaining halves of #4, #6,
  #7 and #8.
- No catch-up stepping: a machine that cannot draw a frame in 33 ms runs the sim
  slower than 30 Hz (the probe reports `BEHIND`).
