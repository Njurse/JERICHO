# Level Hacks

**DISABLED and dropped from the shipped set.** The take-a-ride Singleplayer /
Multiplayer prompt now belongs to the multiplayer module's own `mp.mode` menu
(`JERICHO/MODS/mp/`), which asks the same question AND decides the map the
session loads; because this module answered the same city confirm, running both
would stack two prompts on one press. `JERICHO/CONFIG/modlist.ini` sets
`levelhacks = 0` and it is no longer in `JERICHO_RELEASE_MODS`, so a release
does not carry it. The module is kept in the tree for its Driver 1 work.

Take-a-Ride level hacks, built as a JERICHO module. After the frontend's
take-a-ride **city confirm**, it intercepts the start, asks **Singleplayer or
Multiplayer**, and (for Multiplayer) shows the four cities' small multiplayer
maps and boots the chosen one in *single-player* take-a-ride — so the small
arena maps are testable without a second pad.

The whole module is `levelhacks.c` (plus `mod.toml`); it has **no config keys**
and no settings file — it acts whenever a Take-a-Ride game starts.

## How it works

It listens to five JERICHO events (`levelhacks.c:197-201`):

| Event | Handler | What it does |
|---|---|---|
| `JER_EVENT_FRONTEND` | `LevelhacksOnFrontend` (`levelhacks.c:56`) | On a `GAME_TAKEADRIVE` confirm, sets `a->defer = 1` (`:73`) so the frontend freezes, and opens the Singleplayer/Multiplayer menu. It re-defers on *every* confirm until the mod starts the game, so the stock take-a-ride cannot slip through. |
| `JER_EVENT_FRAME` | `LevelhacksOnFrame` (`levelhacks.c:79`) | Drives the menu from pad 0 (`Pads[0].mapped` / `.mapnew`). On a choice it either continues the stock start (`SetState(STATE_GAMESTART)`, `:101`) or opens the 4-city map list. |
| `JER_EVENT_DRAW_OVERLAY` | `LevelhacksOnDraw` (`levelhacks.c:131`) | Draws the prompt and items (`PrintString` / `SetTextColour`, `draw.h`). |
| `JER_EVENT_GAME_START` | `LevelhacksOnGameStart` (`levelhacks.c:159`) | Re-arms the prompt (`gLhStage = 0`) for the next visit to the frontend. |
| `JER_EVENT_LEVEL_LAUNCH` | `LevelhacksOnLevelLaunch` (`levelhacks.c:176`) | Swaps the pending mission number for the multiplayer-map variant (see below). |

The two menus are static item lists: `gLhPromptItems` = `Singleplayer`,
`Multiplayer` (`levelhacks.c:43-46`) and `gLhMpItems` = `MP Chicago`,
`MP Havana`, `MP Las Vegas`, `MP Rio` (`levelhacks.c:48-53`). They are driven by
the `jer_menu` API (`jer_menu_begin` / `jer_menu_update`, `jer_menu.h`) with
D-Pad up/down and Cross.

### Why it swaps the mission number

The small multiplayer maps (`MLEVELS`/`MNLEVELS`) are chosen by the mission
file's `region` field (`region != 0` → `gMultiplayerLevels` →
`CITYTYPE_MULTI`) — but `LoadMission()` recomputes that flag from the mission
header, so simply setting `gMultiplayerLevels` in the frontend is overwritten
(`levelhacks.c:9-16`). Instead the mod swaps the **mission number** at
`JER_EVENT_LEVEL_LAUNCH` (`levelhacks.c:190`, `a->missionNumber += 8`):

- single-player take-a-ride missions are **M50..M57** — the full city,
  `region == 0`;
- the multiplayer-map variants are **M58..M65** — the same city/night on the
  small `MLEVELS` map, `region != 0`.

`+8` maps `M50 + city*2 + night` → `M58 + city*2 + night`, i.e. the same
city/night the player picked. `NumPlayers` is forced to **1** (`levelhacks.c:118`)
and checked as such before the swap (`levelhacks.c:187`), so it stays
single-player and needs no second controller.

## Enabling

The checked-in `JERICHO/CONFIG/modlist.ini` now pins `levelhacks = 0` (see the
note at the top), so the prompt does NOT appear: mp's `mp.mode` answers that
city confirm instead. To run this module again, set it back to 1 there — and
take mp's `mp.mode` prompt out of the same confirm, or both will open on one
press.

```
levelhacks = 1
```

`JERICHO/MODS/levelhacks/mod.toml` ships `default-enabled = false` (2026-10-12: a
dev module no release ships), so a tree that does not list the module leaves it off;
there is an explicit `levelhacks = 1` above for turning it on. There are no per-module
tunables — the behaviour is
unconditional once the module is enabled, and only fires on a `GAME_TAKEADRIVE`
level with one player.
