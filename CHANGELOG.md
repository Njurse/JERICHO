# JERICHO changelog

User-facing changes per release. Engine/module internals are documented in
[HOOKS.md](src_rebuild/Game/C/JERICHO/docs/HOOKS.md); the inherited REDRIVER2 history
is in [changelog.txt](changelog.txt).

## v0.10.4

### Headline fix: the player car no longer drives at "fast-forward" speed
Since v0.10.3 your own car was moved **twice per physics step** whenever no other car
was close, and dropped back to normal speed the moment traffic came alongside. It
was a missed condition in `GlobalTimeStep` (`handling.c`) left by the 32-car change,
not the clock and not the handling data. The player car is moved once per step
again, exactly as stock.

### Internet play: discovery server and dedicated server
Landed Oct 16-17 and included in the refreshed v0.10.3 downloads; listed here for
anyone updating from an earlier v0.10.3 or v0.10.2 install.
- **Online server browser** - *Multiplayer -> Online* lists servers from the
  discovery server (server name, players, game mode, city, ping) and joins one
  directly, with a *Manual address* row as a fallback. LAN discovery still works.
- **Discovery server by default** - installs point at the project's discovery server
  (`http://jerichomp.animetiddi.es:8100`). A hosted session registers itself and
  heartbeats its player count; set the address empty for LAN-only.
- **Dedicated server ships in the packages** - leave a match running without a
  player hosting: config checks, a mod policy with readable refusals, a console with
  chat, a message of the day for each joiner, and `--advertise-host` / advertised
  port for servers behind NAT or docker. Updates keep your `dediserver.json`. The
  discovery (master) service itself is not shipped.
- **16-player lobbies** - up to host + 15, a paged lobby, and a fixed traffic pool so
  the city doesn't empty as players join. Joiners spawn on a random road point facing
  along the road. The protocol version changed: **everyone must update together**.
- Hosting from the in-game menu is not available yet (#20); host with the
  `mp_host` / `PLAY_HOST` scripts or a dedicated server.

### Other fixes in this release
- **Exactly 30 sim steps per second on any display.** The step gate keeps the
  leftover time from each frame, so high-refresh monitors no longer run slightly slow
  and players on different refresh rates stay in step. After a stall (loading,
  alt-tab) the game resumes at normal speed instead of rushing to catch up.
- **Multiplayer:** fast-forward (BACKSPACE, map screen) is locked for the whole
  session, including level load and the countdown.
- **Take a Ride after online play:** a player who joined someone else's match and
  quit back to the menu could not start a single-player Take a Ride (the game still
  treated them as in the old session). Leaving a match now ends the session.
- **Credits page:** *Options -> JERICHO -> Credits* now opens a credits screen (who
  made JERICHO, and thanks to the REDRIVER2 / OpenDriver2 authors) instead of a few
  console lines.
- **Compile Mods is hidden in the Release build** (it stays in `Release_dev`, where
  modders use it).
- **Mod loader:** a `mod.toml` `id` can no longer point the loader at a library
  outside the module's own folder (part of #4).

### Build, release and diagnostics
- Build dependencies (premake, SDL2, OpenAL, libjpeg) are downloaded over https and
  checked against pinned SHA-256 hashes, in CI and in the setup scripts (part of #8).
- A numbered release is published only when the docs/debug/multiplayer checks pass.
- `jer_test` now fails when it should; setup scripts and CONTRIBUTING match the tree
  (part of #6, #7).
- `JERICHO_HANDLING_PROBE=1` reports `mbc=N/30` (seconds with another car near),
  `resyncs=N`, and a `BEHIND` marker below 28 steps/s.

### Known issues
- Open: #2, #3, #5, #11, #13, #17, #18, #19, #20, and the rest of #4, #6, #7, #8.
- A machine that cannot draw a frame in 33 ms runs the sim below 30 Hz (no catch-up
  stepping; the probe reports `BEHIND`).
