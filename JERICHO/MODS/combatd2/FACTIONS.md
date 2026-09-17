# Factions — the five teams

Caine's Crossfire gives every car a **team identity**: a name it is announced
by, a colour it is drawn in, and (later) its own crew, vehicle and special. The
reference is Twisted Metal (2012), where each faction has a leader, a colour, a
signature vehicle and a roster of minions — and where one character assembles
the field and watches it rather than competing. That last role is Caine's here.

Source: `factions/factions.h` (the types and the API) and `factions/factions.c`
(the five rows, the stance table, the roster and the per-car assignment).

## 1. The five rows

The whole system is this table. `competes = 0` means the faction is a host: it
has a name, a colour and a motive, but it never drives — `cd2FacRosterFaction`
cannot return it and `cd2FacPlayerFaction` refuses it.

| id | tag | full name | title | colour | rank | competes | cars | special | battlecry |
|---|---|---|---|---|---|---|---|---|---|
| TANNER | `TANNER` | John Tanner | Rogue Undercover | `#E6EEF8` ice white | LEADER | 1 | 1 (the player) | MISSILE | "You're coming down with me." |
| MCKENZIE | `MCKENZIE` | Lt. McKenzie | The Law | `#2F6BFF` police blue | LEADER | 1 | 1 | SMG | "Tanner! Stand down!" |
| VASQUEZ | `VASQUEZ` | Vasquez | Enforcer | `#35D06A` toxic green | MINION | 1 | 2 | SHOTGUN | *(silent)* |
| JERICHO | `JERICHO` | Charles Jericho | Chaos / Free Agent | `#FF8A1E` chaos orange | LEADER | 1 | 1 | SPECIAL_JERICHO | "Right in the face, boss!" |
| CAINE | `CAINE` | Solomon Caine | Organized Crime | `#E02424` mafia crimson | HOST | **0** | 0 | MINE | "Everybody gets what they want. Eventually." |

Each row also carries the story's **motive** — what that faction wants out of
Caine's tournament — which the field dump prints and a later ending/story pass
can read.

Two deliberate values:

- **Vasquez has an empty battlecry.** He is entirely silent in canon, so the
  empty string is the faithful value rather than a placeholder. The dump prints
  it as `cry: ` instead of substituting a dash.
- **Vasquez is a MINION, not a leader.** He was demoted from faction leader for
  the same reason: he has almost no direct story presence. His two cars are the
  muscle of the field. That is also the one place a faction fields more than one
  car, which is why messages number the members — `VASQUEZ` and `VASQUEZ 2`.

### Colours, and where they are used

Colours are plain RGB bytes on the row (`r, g, b`), plus an unused `accent`
(`ar, ag, ab`) reserved for badges. **This is NOT the engine's packed colour
word** — there is no `B<<16|G<<8|R` packing anywhere in this feature; each
channel is a separate byte, the same convention `CD2_WEAPON_DEF.colR/G/B` and
`SetTextColour` already use.

Live uses today: the kill banner's names (`combatd2wreckfx.c`) and the opponent
map blips (`cd2AiOnDrawMap`). The palette was chosen to stay readable as HUD
text against the dark city palette.

## 2. The field: who drives what

`TANNER` is the **player's** faction (config `player_faction`). The four AI cars
are the roster in `factions.c`:

| spawn index | faction | cars |
|---|---|---|
| — | TANNER | the player's car |
| 0 | MCKENZIE | 1 |
| 1 | VASQUEZ | 1st of 2 |
| 2 | VASQUEZ | 2nd of 2 |
| 3 | JERICHO | 1 |

The order lives in the `sRoster` table, not derived from `carsPerLevel`: a host
faction must never be able to reach the field, and the spawn order is the AI's
own index (`cd2AiSpawnOne`'s `index`). `cd2FacDumpField` cross-checks the two
descriptions and logs a `WARNING` line if they disagree.

## 3. The stance table

`sStance[a][b]` answers how `a` treats `b`. **Every pair is HOSTILE** — each
faction wars with each other, a free-for-all. `ALLY` and `NEUTRAL` are kept in
the enum so that an alliance/betrayal pass is a table edit rather than a
rewrite (Caine's deals, Jericho's turn, Vasquez's secret loyalty all belong
there).

Anything that is **not** a faction (`CD2_FAC_NONE`) reads as HOSTILE — an
unrostered car is nobody's friend. Nothing consumes the table yet; see §5.

## 4. How a car gets its faction

Storage is `gCd2CarFaction[MAX_CARS]` in `factions.c`; `CD2_FAC_NONE` means "not
assigned yet". Three lifecycle handlers (`cd2FacRegister`, called from the
module entry **before** `cd2AiRegister`) fill it in:

- **GAME_START** — clears every car and logs the field plan.
- **CAR_STEP** — derives the player car's faction the first time the car is
  stepped (the player car does not exist yet at `GAME_START`; the AI's own
  level-start note says the same), and names any opponent that reaches the sync
  without a roster slot as VASQUEZ rather than leaving it anonymous.
- **RESET_CAR** — re-derives the player car's faction only. Identity is not
  damage state, so a respawned car keeps its team.
- **the AI spawner** — `cd2AiSpawnOne` calls `cd2FacAssignAiCar(slot, index)` at
  the point the car is committed to the field, so an opponent can never be
  nameless in a message.

## 5. Live vs inert

Live in this pass: the registry, the roster, the per-car assignment, the stance
lookup (`cd2FacStance` / `cd2FacAtWar`), the colour lookup, the coloured names
in the kill banner, and the faction-coloured map blips.

**Defined but read by nothing** (deliberately — this pass is the data and the
naming):

| attribute | what would read it |
|---|---|
| `stance` table | `cd2AiFindTarget` (`ai/opponent.c`) — skip allies, engage neutrals only when provoked |
| `carModelSlot`, `carPalette` | `cd2AiSpawnOne` — give each faction its signature vehicle instead of a random resident slot |
| `driverPedModel`, `gunnerPedModel` | `weapons/core/crew.c` (the ped spawn is hardcoded to `TANNER_MODEL` today) |
| `specialWeapon`, `battlecry` | the spawn-time arsenal + a HUD line on firing the special |
| `loyalty`, `aggression` | the AI's temperament (`cd2AiBravery` and friends) |
| `motive` | a story/ending pass |

**The crew-model caveat.** `driverPedModel` and `gunnerPedModel` are two
separate fields holding `TANNER_MODEL` (0) for every faction *on purpose*: ped
model 1 has no bone-draw path yet — `motion_c.c` gates the skeleton/pose work on
`pedType == TANNER_MODEL` — so a faction-specific crew body needs that gate
opened first. Untick that and the whole thing is a two-field data edit.

## 6. Making it behave differently

- **A different palette** — edit `r, g, b` on the row.
- **A different player team** — `combatd2.ini`: `player_faction = <id>` (0
  TANNER, 1 MCKENZIE, 2 VASQUEZ, 3 JERICHO; CAINE is refused because he does not
  drive). `factions = 0` turns the system off entirely and restores the old
  anonymous wording ("Flanker was killed by ...").
- **The player's own name in the banner** — `cd2cAnnounce` in
  `combatd2wreckfx.c` returns `"You"` for the local player. That single return
  is the switch: hand back the tag instead and the line reads `TANNER`.
- **Alliances** — one value per pair in `sStance`.

## 7. Reading a run

The field plan is logged unconditionally at `GAME_START` (it doubles as the
consistency check), and each car's assignment is logged when
`[combatd2] debug_log = 1`:

```
[combatd2] factions: 5 rows, player=TANNER, 4 AI cars rostered
[combatd2] faction 0 TANNER   John Tanner     Rogue Undercover  rgb=E6EEF8 competes=1 rank=0 cars=1/1 special=MISSILE
[combatd2] faction TANNER wants: Caine's criminal network exposed - ... | cry: You're coming down with me.
...
[combatd2] faction: car=7 -> VASQUEZ (Vasquez)
```

`grep '\[combatd2\] faction'` on `REDRIVER2.log` after a run is the whole
feature's evidence. The HUD colouring itself needs eyes — boot an arena
(`tools/arena_test.sh`) and wreck someone.
