# JERICHO changelog

User-facing changes per release. Engine/module internals are documented in
[HOOKS.md](src_rebuild/Game/C/JERICHO/docs/HOOKS.md); the inherited REDRIVER2 history
is in [changelog.txt](changelog.txt).

## Unreleased

- **Dedicated server chat commands and admins.** Lines starting with `!` or `/` are
  commands, answered privately and not shown to others: `!help`, `!whoami`, `!who`,
  and for admins `!kick`, `!cops on|off`, `!difficulty 0-2`, `!motd`, `!say`. Admins are
  listed by player id in `dediserver.json` (`!whoami` shows yours) or log in with an
  optional `admin_password` (`!login`, rate limited).
- **Per-install player id.** The game generates a random id once (`[mp] player_id`)
  and sends it when joining, so a server can recognise you. Older servers ignore it;
  no protocol change. It is a bearer secret, not a login.
- **Dedicated server plugins.** The server is now a small core plus plugins
  (`commands`, `admin`), and you can add your own in its `plugins/` folder (see the
  dediserver README; `example_hello.py` shows how).

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
- **Credits page:** *Options -> JERICHO -> Credits* now opens a two-column credits
  screen - JERICHO by Jaret (Njurse), special thanks to Commando52Colton (advice on
  palette and texture paging, mission script research), and thanks to the REDRIVER2
  / OpenDriver2 authors - instead of a few console lines.
- **Multiplayer chat is no longer cut off.** A dedicated server's long message of
  the day (and its `say` lines) now arrive whole, split over several chat lines
  instead of being cut at 95 characters; chat rows wrap inside the screen instead of
  losing their last letters at the right edge; a long line you are typing shows its
  end rather than running off screen. The chat area and input line sit a little
  higher, clear of the bottom edge.
- **Attract-mode demos no longer softlock the game.** When the idle demo's replay
  (`REPLAYS/ATTRACT.n` from your Driver 2 data) is missing or unreadable, the game
  goes back to the menus instead of hanging on the loading screen.
- **Version shown:** the window title is the full build version (for example
  `JERICHO 0.10.4`, or `JERICHO 0.10.4-3-gabc1234-dev` for a dev build; `-dirty`
  only on a build from modified sources), and the Options screen shows it small at
  the bottom-left.
- **Driver 1 cars no longer pull to one side.** Cars ported from Driver 1 (d1cars)
  could have the two wheels of an axle placed unevenly, or a centre of gravity off the
  centre line, left over from re-proportioning a Driver 2 donor. They are now made
  left/right symmetric as they load (stock Driver 2 cars untouched), and the bake no
  longer produces the offset. The log says `d1cars: symmetrized ...` for each car it
  corrected.
- **Take a Ride is always called Take a Ride.** The server browser, join log and
  advertise log named the mode from the cops setting ("COPS N ROBBERS" with cops on);
  they now name it from the server's game mode, so cops on/off no longer renames it.
- **Release re-cuts refresh their notes.** Moving a release tag used to leave the old
  "What's new" text on the existing GitHub Release; the publish job now rewrites that
  section from CHANGELOG.md every time.
- **Your own chat shows up on a dedicated server.** The dedicated server relayed a
  player's line to everyone except the sender, but the game only prints its own
  line when the server echoes it back, so on a VPS/dedicated server you never saw
  what you typed. It now echoes to everyone; each received line is also logged
  (`[mp] chat from ...`).
- **Dedicated server rules are readable and default to cops on.** `dediserver.json`
  takes `"cops"`, `"cop_difficulty"` (0..2), `"cop_immunity"` and `"invulnerable"`
  (with matching `--cops/--no-cops`, `--cop-difficulty N`, `--cop-immunity`,
  `--invulnerable` flags) instead of a packed `"rules"` number, and `status` shows
  them in words. Existing `dediserver.json` files with `"rules": 0` are kept by
  updates and still mean cops off until you change them.
- **Map arrows in player colours:** each player's arrow on the map uses their
  preferred car colour (yours too, when you have one set).
- **Police light bars follow the car:** a cop car imported from another city
  flashes in its own city's light-bar pattern and colours instead of the host
  city's. (Siren sounds still come from the host city's bank.)
- **Options version text** is now fully on screen and white; the Credits text is
  a size larger.
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
- `JERICHO_HANDLING_PROBE=1` reports `mbc=N/30` (steps in the last second with another car near),
  `resyncs=N`, and a `BEHIND` marker below 28 steps/s.

### Known issues
- Open: #2, #3, #5, #11, #13, #17, #18, #19, #20, and the rest of #4, #6, #7, #8.
- A machine that cannot draw a frame in 33 ms runs the sim below 30 Hz (no catch-up
  stepping; the probe reports `BEHIND`).
