# JERICHO changelog

User-facing changes per release. Engine/module internals are documented in
[HOOKS.md](src_rebuild/Game/C/JERICHO/docs/HOOKS.md); the inherited REDRIVER2 history
is in [changelog.txt](changelog.txt).

## Unreleased

## v0.10.7

*Released 2026-10-18.*

A small online update. Respawning now puts you back on the road a short drive away
instead of on the spot you crashed, Restart is gone from online games, and players
already in a match should no longer be reset when someone joins. Server admins also get
private `!login` and configs that pick up new settings by themselves.

### Highlights

- **Respawn somewhere new.** Respawn from the pause menu, or after a wreck, now puts you
  on a clear road roughly 60–150 m away, facing along the road and away from other
  players. If there is no such road nearby, you go to the nearest road as before.
- **No more Restart in online games.** Restart, Retry and Play Again are no longer in
  the pause and end-of-ride menus during an online game, where they could get stuck.
  Use Respawn on the Multiplayer page instead.

### Fixes

- **Joining shouldn't reset other players any more.** When someone joined, players
  already driving could be put back on a road near the start with their damage and
  wanted level cleared. A new player's car can no longer take over a car someone is
  driving. If it still happens to you, please send us your log from that session.
- **`!login` passwords stay private.** A `!login` line (or any `!` command) is never
  shown to other players, and the server log shows it as `!login ****`. In games hosted
  from the game itself, `!` command lines are no longer sent to other players.

### For server admins and updating

- **Update your server for this release.** Pull the latest server files and restart
  `dediserver.py` (and `masterserver.py` if you run one). That brings the `!login`
  privacy fix and config auto-upgrade:
  - On start, the server adds any new settings to your `dediserver.json` or master
    config, with their default values, and logs which ones it added.
  - Your existing settings are never changed or removed. The old file is kept as
    `.bak`, and a config with nothing missing is left untouched.
- No protocol changes: v0.10.6 and v0.10.7 players can play together. The in-game
  updater offers v0.10.7 as usual.

## v0.10.6

*Released 2026-10-18.*

Car colours are fixed and you can now pick them before you drive. Changing car no longer
leaves you with broken colours, and the Take a Ride car select has a Palette picker.

### Highlights

- **Pick your car's colours in Take a Ride.** The car select has a new Palette row
  (Left/Right) showing n/N for the selected car. Single-palette cars, such as every
  Driver 1 car, show 1/1. The car spawns in the palette you pick.

### Fixes

- **Car colours no longer break after Change car.** The pause menu's Change car now checks
  the palette you chose on one car against the car you switch to. A car with a single
  palette (every Driver 1 car, specials, cops) uses its own colours, and the Palette row
  shows 1/1 there. Your choice comes back on a car that has it. The same check covers
  spawns, respawns, and the car kept across a map change or reconnect.
- **Other players' cars always draw correctly.** If a peer or an older client sends a
  palette its car doesn't have, that car is drawn in its own colours instead of broken ones.

### For server admins and updating

- No protocol or config changes. Mixed v0.10.5 and v0.10.6 clients and servers can play
  together. The in-game updater picks up v0.10.6 as usual.

## v0.10.5

*Released 2026-10-18.*

### Highlights

- **Update checker and one-click update.** At start the game checks for a new
  release in the background and shows what changed. *Update JERICHO (Restart)*
  downloads it, checks its SHA256, installs it over your folder (keeping your
  settings) and restarts the game. You can also say *Remind me next launch* or turn
  the check off.
- **Dedicated server grown up.** Chat commands (`!help`, `!who`, `!kick` with a
  reason, `!cops`, `!difficulty`, `!motd`, ...), admins by player id or password,
  plugins, time of day and weather, and `!vote <city>`. Everyone reconnects
  automatically when the city changes.
- **On-foot multiplayer.** Players on foot walk and run properly for everyone, the
  dedicated server relays them, you can warp to someone on foot, and being busted or
  drowned on foot no longer ends your match.
- **Pick your car's colour.** Choose a palette in Change car or press SQUARE on the
  car select. Everyone sees the colour you picked.
- **Dedicated server parity.** Cars show straight away, cops follow the server's
  setting, ping, rx/tx and loss are real, and joins show the player's version and mods.
- **Chat.** Typing no longer drives your car. Chat shows 10 lines, keeps its history
  while you type, pages with PgUp/PgDn, and `chat_hold_seconds` sets how long lines stay.

### Fixes

- Cops: other players crashing into walls no longer raises your felony; cop cars
  no longer get corrupted meshes; the siren works after Change car.
- Live joiners spawn on a road away from others; respawned cars face along the road.
- Leavers' cars and on-foot players are cleaned up; nametags stay visible at the
  sides of a widescreen.
- The window title shows the release version again.

### For server admins and updating

- **Back up `JERICHO/CONFIG/` and `config.ini` before updating** (good practice; the
  updater keeps them).
- `config.ini` is no longer overwritten: releases ship `config.default.ini`, and the
  game creates `config.ini` from it when you have none.
- `dediserver.json` is no longer shipped or tracked. The server creates it from
  `dediserver.example.json` on first start; an existing one is left alone.
- **Updating from v0.10.4:** that version's `update_jericho` can break halfway through
  when it overwrites itself. Copy it somewhere else and run the copy, or just
  download v0.10.5 by hand. From v0.10.5 on, the in-game updater and the scripts
  handle this.

### All changes

- **Update check.** After the loading screen the game checks GitHub once, in the
  background (no wait when offline), and when a newer release is out shows its changes
  before the main menu: *Update JERICHO (Restart)* downloads, verifies and installs it
  with `update_jericho` and restarts the game; *Remind me next launch*; or *Go away and
  leave me alone* turns the check off (`update_check=0` in `JERICHO/CONFIG/jericho.ini`).
  It also tells you when the release you have was **re-cut** (rebuilt under the same tag
  since you installed it). A `Release_dev` build updates to the `Release_dev` download.
  Dev builds can force the screen with `-forceupdatecheck` (README, dev tooling).
- **Your `config.ini` is no longer overwritten by a new download.** Releases ship
  `config.default.ini`; the game creates `config.ini` from it when you have none.
- **`update_jericho` keeps your flavour** (a `Release_dev` install updates to
  `Release_dev`), records what it installed in `JERICHO/CONFIG/installed.json`, and no
  longer trips over its own replacement mid-update.
- **Join toast.** A host now shows who joined with their version and a short mods
  checksum, e.g. "Bob joined (v0.10.4-dev, mods 3fa2c1)"; a dedicated server announces
  the same in chat. Older clients show "?" for the version. No protocol change.
- **The window title shows the release version again**, not the name of the latest
  non-release tag (`git describe` now only considers `v*` tags).

- **Remote on-foot players walk properly.** A remote Tanner no longer gets stuck in
  the walking-backwards animation: it now mirrors the owner's heading and speed and
  plays the matching idle, walk or run clip (reverse only when the owner backs up),
  and stands idle if their poses stop arriving.
- **Leavers are cleaned up.** When a player leaves or drops, their on-foot Tanner and
  the car they last got out of are removed for everyone, unless another player has
  taken that car. The dedicated server now sends the updated roster at once.
- **Faster late-join placement.** A live joiner's car is placed on the road on the
  first frame it exists (retrying while roads stream in) with the camera snapped,
  instead of sitting on the host for 15 frames and then jumping.
- **Dedicated server chat commands and admins.** Lines starting with `!` or `/` are
  commands, answered privately and not shown to others: `!help`, `!whoami`, `!who`,
  and for admins `!kick`, `!cops on|off`, `!difficulty 0-2`, `!motd`, `!say`. Admins are
  listed by player id in `dediserver.json` (`!whoami` shows yours) or log in with an
  optional `admin_password` (`!login`, rate limited).
- **Per-install player id.** The game generates a random id once, keeps it in its
  own obfuscated, checksummed file `JERICHO/CONFIG/player.id` (an older `[mp] player_id`
  is moved there; a damaged file is kept as `player.id.bad` and a new id made), and sends it when joining, so a server can recognise you. Older servers ignore it;
  no protocol change. It is a bearer secret, not a login. Read yours with `!whoami`, the
  game log, or `python JERICHO/MODS/mp/tools/player_id.py`.
- **Dedicated server relays on-foot players and traffic.** It dropped both message
  types, so on a dedicated server nobody saw a player walking around, nor the cars
  and police another machine was driving. It now forwards them like a game host does.
  Car bumps (to the player hit), car colours (the whole table, late joiners included)
  and mods' network channel are forwarded too.
- **Car palette choice.** Change car menu (pause > Vehicle Options) has a
  "Palette: N/6" row (left/right) that repaints your car at once; the frontend car
  select cycles it with SQUARE (a notice shows "Palette N/6"). Saved as `[mp] palette`
  in mp.ini and used for the next spawn. Palettes a city never filled fall back to the
  car's base colours.
- **Palette sync (#31).** The car pick (JPCC) now carries the palette (MP_CAR
  reserved[0], flagged by reserved[1]; no protocol bump) and the dedi keeps the HELLO /
  pick palette for the roster, so late joiners see the right colour before the owner's
  first carstate; the host's roster follows the carstate palette too.
- **Nametags at the screen sides (widescreen).** A tag is kept while its car is still
  partly on screen (pinned to the edge) instead of vanishing when the car's centre
  reaches the edge.
- **Corrupted car meshes until respawn (D1 cop cars).** A carhacks hot load that builds
  a model into a resident slot now rebuilds the drawn vertex buffer of every car already
  on that slot (keeping damage); before, a car pinged in before the import landed kept
  the old model's vertices under the new polys. As a safety net, the existing
  "STRETCHED VERTEX" check now rebuilds a car's mesh the way a respawn does.
- **MP: no game over on foot.** Being busted by a cop, drowning or a train while on
  foot opened the game-over menu over a session (camera lost its target, rejoin
  needed). It is refused like the in-car wreck already was: not dying, felony cleared,
  pulled out of the water to the level start if drowning.
- **MP: live joiners spawn on a road away from others**, not on the level start where
  the host came in: a random road spot 1.5-4.5k units out, clear of other players'
  cars by 2000 units, facing along the road (falls back to the start and logs).
- **MP: respawned cars face along the road** (the road's direction nearest the car's).
- **MP: an on-foot pose relayed by the in-game host must describe its sender** (the dedi
  already enforced this).
- **`chat_hold_seconds`** in `[mp]` of mp.ini (2..120): how long a chat line stays at
  full brightness before fading. Default **12 s (was a fixed 20 s)**; new engine call
  `jer_console_set_chat_hold()`.
- **Auto-reconnect keeps a car picked by slot**: a car-select slot is per city, so the
  rejoin now remembers the model on the road instead of skipping the restore.
- **Auto-reconnect on a server city change.** A passed `!vote` now kicks with a rejoin
  flag (KICK `reserved[0]`, no protocol bump); the client says "Server changing city -
  reconnecting...", re-joins the same server through the normal join (retries ~20 s,
  Esc cancels), loads the new city and restores its previous car (model + city, via
  Change car; falls back to the default and logs if it can't). Normal kicks never
  reconnect. Plugin API: `kick(..., rejoin=True)`. Car colour is not carried over.
- **Kick reasons.** `!kick <slot|name> [reason]`, console `kick <id> [reason]` and the
  plugin `kick(player, reason=None)`: the kicked player sees "Kicked by admin: <reason>"
  (an optional 64-byte text tail on the KICK frame, cut to 63 bytes on a UTF-8
  boundary; old clients still get the stock message), chat is told and the log keeps it.
  No reason = unchanged. The in-game host's pause-menu kick is unchanged (no text entry).
- **Typing in chat no longer drives the game.** While the MP chat prompt is open, all
  pad/keyboard game input (driving, horn, handbrake, camera, pause...) is blocked via the
  new engine call `jer_input_block()`; keys held when it closes are ignored until
  released. The car coasts; other players still see it move.
- **Chat shows 10 lines** (was 8), so a server `!help` fits; PgUp/PgDn page by 9.
- **`dediserver.json` is no longer tracked.** The repo and packages ship
  `dediserver.example.json`; the server creates your `dediserver.json` from it on
  first start and never overwrites one, so pulls and updates keep your settings.
  **Git checkouts: back up your `dediserver.json` before pulling this change**
  (pulling deletes an unmodified tracked copy; see the dedicated server README).
- **Dedicated server: `!vote <city>`.** Players vote the server onto another city; on a
  majority it saves the city, updates the listing and asks everyone to rejoin.
- **Siren works after Change car.** Changing into a police car lit the light bar but the
  siren stayed silent until you got out and back in; the car's sounds now follow the change.
- **Chat history.** While typing a chat message, the last chat lines show again even
  if they had faded, and Page Up / Page Down scroll back through the last 48 lines.
- **Scoreboard CAR column names the car.** It showed the level's resident slot
  ("car 3"); it now shows `city:model`, the way car selection identifies a car.
- **Dedicated server shows a player's car straight away.** It now handles the car pick
  like a game host: the roster lists the chosen car at once instead of after the
  player's first position update.
- **Scoreboard rx / tx / loss for every player.** The host (or dedicated server) now
  shares each player's link stats in a new optional message, so the columns show
  values instead of "--" for other players. Older games ignore it. A dedicated server
  reports loss as unknown ("--").
- **Dedicated server measures ping.** It sent 0 ms for every player in the roster;
  it now pings each client and reports the round trip, like a game host.
- **Warp to a player on foot.** "Warp to player" refused anyone out of their car; it now
  lands you beside where they are standing.
- **Multiplayer cops fixes.** Another player crashing into walls or fences no longer
  raises *your* wanted level. Your patrol cops are no longer deleted when another
  player is nearby. With cops on, each machine keeps one traffic slot free for a cop,
  so police still turn up when the streets are full.
- **Dedicated server now applies its cops setting.** It sent "no rules" to every
  player, so cops were off whatever `dediserver.json` said; the configured rules
  now reach players.
- **Time of day and weather for dedicated servers:** `"time_of_day"` (dawn/day/dusk/
  night) and `"weather"` (clear/rain/wet), plus admin `!time` / `!weather` (from the
  next level load).
- **More admin tools:** `!admin add|remove|list` (saved to `dediserver.json`),
  `!motd save`, `!help <command>`, join/leave announcements, and admin actions are
  logged with who ran them. Player ids are shown as standard UUIDs and created at
  first game launch.
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
