# Level Hacks

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

Enable the module in `JERICHO/CONFIG/modlist.ini`:

```
levelhacks = 1
```

`JERICHO/MODS/levelhacks/mod.toml` ships `default-enabled = true`. The
checked-in `JERICHO/CONFIG/modlist.ini` already sets `levelhacks = 1`, so the
prompt appears on the next Take-a-Ride. There are no per-module tunables — the
behaviour is unconditional once the module is enabled, and only fires on a
`GAME_TAKEADRIVE` level with one player.
