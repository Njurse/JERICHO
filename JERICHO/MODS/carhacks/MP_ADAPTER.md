# MP_ADAPTER.md — carhacks over the JERICHO net bridge

How carhacks and the multiplayer module (`JERICHO/MODS/mp/`) fit together: the car
**identity** schema, the **channel** carhacks multiplexes over the bridge mp
exposes, the host-authority rule, and the mp-side deltas (1 and 2 are now landed;
3, the in-session roster, is still open).

## The problem this exists to solve

Two facts, both measured:

1. **mp carries a car as a bare MODEL NUMBER.** `MP_ROSTER_ENTRY.model` and
   `MP_CARSTATE_ENTRY.model` (`mp_proto.h`); the slot ↔ model resolution happens
   once, host-side, in `MpPlayerCarModel` using the session city
   (`mp_session.c:363-382`). `carSlot`/`carId` are documented as informational.
2. **A model number is not an identity.** Every city ships `CARMODEL_0..12` and the
   same number is a *different* vehicle in each (`VEHICLES.md`). So "Rio's model 9"
   is not expressible on mp's wire at all.

On top of that, the engine reads a level's car data from **one** foreign city
(`models.c`, `gCarModelSource`), so a session has to *agree* which city that is.
Today it is whatever each machine's own `carhacks.ini` happens to say.

## The identity schema — `carid.h`

```
CHK_CAR_ID { unsigned char city; unsigned char model; }   city 0..3, or CHK_CITY_NATIVE (0xFF)
wire: [city][model]  (2 bytes)   CHK_CAR_WIRE_VERSION 1
```

`CHK_CITY_NATIVE` means "the level's own city" — no import, the stock path. The
wire form is versioned so a peer that does not know the version *refuses* rather
than guessing. `carid.h` is the single place that defines this; everything else
(`carimport.c`, `net.c`, `carselect.c`) is written against it.

## The connection schema — `net.{c,h}`

carhacks registers its own channel on the bridge mp already exposes
(`jer_net.h`), so **no mp change is needed for the transport**:

| | |
|---|---|
| channel | `carhacks.car`, `JER_NET_RELIABLE` |
| inbound | `JER_EVENT_NET_RECV`, matched on the channel name |
| size | ≤ 27 bytes payload (the bridge's cap is 1024) |
| with no session | every call is a no-op (`jer_net_is_active() == 0`) |

Message layout — `[wireVersion][tag]` then the payload:

```
REQ   (no payload)                     a joiner asks for the set and the car table
PICK  [city][model]                    "this is the car I am driving"
CARS  [count]                          the host's per-player car table, so EVERY
      count x [playerId][city][model]  machine knows who drives what
SET   [guestCity][count][version]      the host's agreed import set
      count x [slot][city][model]
```

`PICK` is no longer gated on the import agreement: identity is needed whether or
not the sets are agreed (only `SET` is). The host answers `REQ` with both.

## Drawing a peer's car

mp's carstate carries `cp->ap.model`, a **resident slot**, so the car one machine
draws for a peer is that peer's vehicle only when both machines' resident tables
agree. Two halves fix that; one limit is left.

**Identity (`CARS`).** Every machine needs to know what every player drives, and a
client's `PICK` only reaches the host - so the host relays the table (above).
`chkNetLocalCar()` reads the local player's car from the ENGINE (`player[0]
.playerCarId` → `car_data[].ap.model` = slot → `residentCarModels[slot]`, with
`GetCarModelSourceCity(slot)` as the city and `CHK_CITY_NATIVE` for the level's
own), so it reports the car actually being driven rather than a menu pick; a FRAME
watcher re-advertises whenever it changes, because mp settles `-mpcar` *after* the
session is up. `chkNetPeerCar(player, out)` / `chkNetPeerCount()` read it back.

**The correction (`JER_EVENT_CAR_PEER_DRAW`).** mp fires this as it applies a
remote car's state (declared in `jer_events.h`, so the engine stays generic).
carhacks answers whether this machine really holds that player's vehicle, and when
it does not it names the palette the car *actually drawn here* supports - 0, the
base colours every resident model has - and mp uses that instead of the owner's
index. A car that IS the owner's is left alone, so mp's owner-authoritative colour
still works. Measured over the divergent pair:

```
[carhacks/net] peer 1 drives RIO model 9, but this machine draws HAVANA model 8
               in slot 5 - using that car's own colours (theirs is not loaded here)
[mp] palette: player 1 reports 2, drawn as 0 (corrected: not their car here)
```

Carrying a nonzero owner palette needs a lever: a headless run has no colour
picker, so the local car always comes out palette 0 and none of this would ever
fire. `CHK_FORCE_PLAYER_PALETTE=<n>` sets it, the way `CHK_FORCE_CAR` drives the
menu.

**The limit - one foreign city per level.** `chkNetFoldPeerCars()` folds every
peer's car into the level's import set, so a peer is drawn as their own vehicle
where the engine can hold it. It cannot when that car comes from a SECOND foreign
city: the engine reads one foreign city per level, and the set refuses the entry
loudly (below). A car from the level's own city is skipped - it needs no import,
and skipping it keeps an identity that has not settled yet from claiming a spare
slot with the level's own model 0.

**Why that is not the whole fix.** A level reads its car files ONCE
(`JER_EVENT_CAR_DATA_SOURCE` → `ProcessCarModelLump`), and a joiner's car is only
known after it joins - so on the machine that loaded first, a peer's car can never
be in the set in time, whatever city it is from. Loading it afterwards is the next
unit.

### The hotload hand-off (next unit)

**Measured first: a second city is not partially supported, it is absent.** The
gate was opened by hand (`two_guest_cities = 1`, a measurement lever in
`carimport.c`) so the ENGINE finally saw a set naming two cities:

```
[carhacks] import: slot 5 <- model 8 from HAVANA
[carhacks] import: slot 6 <- model 9 from RIO          <- the lever let this in
cross-city: car data from HAVANA (131412 bytes of models, 14792 of car palettes)
cross-city: HAVANA car palettes applied to civ_clut rows 8..15
cross-city: slot 5 geometry from HAVANA model 8
                                                       <- no "car data from RIO",
                                                          and no "slot 6 geometry"
[pair] verdict: PASS   (no crash, no dump, 0 dumps)
```

The per-slot source city is real, but a level's car lumps are read for ONE city
(`ProcessCarModelLump`), so a slot pointing at a second city gets no data at all -
quietly. That is what the hotload actually has to do:

1. The identity is already there: `chkNetPeerCar()` says which (city, model) each
   peer drives, and `chkNetFoldPeerCars()` is already the place a car joins the set.
2. Read the second city's **model + page + palette** lumps at runtime (the parts
   `ProcessCarModelLump` does at level start), then pin them with `CarImportPin`
   and upload the palette rows - the single-city assumption is precisely in the
   palette upload, which keys off `GetCarImportCity()` (`cars.c:1623`).
3. Fit them in the measured budget: ~130 KB of models plus ~15 KB of car palettes
   per city (the `cross-city: car data from …` lines, `CROSS_CITY.md`), with the
   CLUT column ~89-95% committed and the level font owning rows 466..511
   (`carhacks/docs/VRAM.md`; the level's own slot walk already reaches y=436). Refuse
   rather than corrupt when it does not fit.
4. Rebuild the peer's car object with the same call the mesh fix uses
   (`CreateDentableCar`, mp's `MpAdoptRemoteCar`). `ChkOnCarPeerDraw` then stops
   correcting that player, because the car drawn IS theirs.

## Authority and lifecycle

The **host is authoritative**, and the reason is the engine's one-guest-city rule:
there is exactly one foreign city per level, so the host's choice has to win for
everyone. `mp_agree_imports = 0` (`carhacks.ini`) turns the whole agreement off —
every machine keeps its own set, which is what a session where the players
deliberately want different cars needs, and what the three-city stress test below
uses.

1. **Session start** (`JER_EVENT_FRAME` sees `jer_net_is_active()` become true):
   the host publishes what it has (possibly nothing yet), a client sends `REQ`.
2. **The set is built** — each machine's `JER_EVENT_CAR_DATA_SOURCE` builds its
   level's import set. The host then **publishes** it (`chkNetNotifySetBuilt`).
   Publishing off `JER_EVENT_LEVEL_LAUNCH` was wrong and is called out in the
   source: that event fires *before* the level's car data is set up, so it sent an
   empty set.
3. **A `PICK`** from a peer is folded into the host's own set (the claim takes a
   spare resident slot, so the host **loads** what its clients asked to drive) and
   the host re-publishes.
4. **`REQ`** from a joiner is answered by the host re-publishing.
5. **A client adopts** the received `SET`: while one is in force its own
   `carhacks.ini` stands down. On the **host** the config is the authority and is
   always read — the stand-down rule must never apply to the authority (that was a
   real bug this caught).

Because the engine applies an import set once per level, **an agreed set only takes
effect at a level LOAD**. A client that has not received one before its own load
falls back to its own config for that level and adopts the host's at the next. A
mid-level change therefore cannot move anyone's car; it takes effect next level.

## Slot ownership — the contract with mp

`carhacks` writes `modelSource[slot]` (which city a resident slot reads from) and
`models[slot]` only for the entries it owns:

| slots | owner | what |
|---|---|---|
| the config `import = slot:city:model` slots | carhacks | that slot's model + source city |
| the pick's / a peer's spare slot (5, then 6) | carhacks | that foreign car |
| 5, 6 when mp seats extra players | **mp** | `gMpExtraModel`, `modelSource = -1` |

**Ownership, resolved.** Both used to use spare slots 5/6 and both answered
`JER_EVENT_CAR_DATA_SOURCE` at priority 0, so one could silently overwrite the other.
Both halves are fixed: carhacks now answers that event at priority 10 — i.e. AFTER mp
— and its automatic slot choosers (`chkImportApplyPick`, `chkNetFoldPeerCars`) skip
any resident slot that already holds a model (`chkImportSlotFree`, fed the engine's
live `models[]` through `chkImportSetEngineModels`). mp's reserved slots win
deterministically, and so does any other module's claim.

## mp-side deltas — 1 and 2 are DONE

1. **Carry the city. DONE** (`MP_PROTO_VERSION` 6). `MP_ROSTER_ENTRY.modelCity` and
   `MP_CARSTATE_ENTRY.modelCity` carry the city (`0xFF` = the session city), and the
   carstate's `model` is the model NUMBER, not a slot. `MpResidentSlotForCar` resolves
   the pair to the receiver's OWN slot, and `-mpcar [city:]model` names one.
2. **Reserved slots. DONE**, by observation rather than a query: carhacks answers the
   source event after mp and skips slots the engine already holds a model in (above).
3. **Offer the roster in-session. STILL OPEN.** carhacks' menu declines while a
   session is live precisely because the stock car screen is mp's. Giving the roster
   row to mp's own car screen — or letting carhacks' Ride drive mp's START — is the
   next unit.

## What is verified today

### The channel, over a real session

Over a real two-instance session on one machine (`JERICHO/MODS/mp/tools/mp_pair.bat`,
host + joiner, `carhacks.ini` = `cross_city_vehicles = 1` / `import = 5:0:8`, host
level RIO):

```
host    [carhacks/net] published the agreed set: guest city CHICAGO, 1 entry
client  [carhacks/net] asked the host for the agreed set
client  [carhacks/net] the host's agreed set arrived: guest city CHICAGO, 1 entry (applied at the next level)
```

and, with no session at all, a plain single-player run logs only the channel
registration — the local cross-city path is unchanged (`devcheck.sh`, all 4
scenarios clean).

### Three cities at once — `tools/chk_mp_foreign.sh`

The stress the adapter exists for: a **different foreign city on each side** than
the level's own, with each player pointed at the imported model (`-mpcar <model>`)
rather than at one of the level's frontend slots. Measured
(`chk_mp_foreign.sh --seconds 35`, level CHICAGO, host HAVANA, client RIO,
`mp_agree_imports = 0` on both so each side keeps its own set):

```
host    import set: level CHICAGO, guest city HAVANA, 1 entry
host    cross-city: car data from HAVANA (131412 bytes of models, 14792 of car palettes)
host    cross-city: slot 5 geometry from HAVANA model 8
host    JERICHO-DIAG: player slot=5 (model=8, want=8, startinfo[0]=8) residents= 1 2 3 0 4 8 11 9

client  import set: level CHICAGO, guest city RIO, 1 entry
client  cross-city: car data from RIO (135688 bytes of models, 15704 of car palettes)
client  cross-city: slot 5 geometry from RIO model 9
client  JERICHO-DIAG: player slot=5 (model=9, want=9, startinfo[0]=9) residents= 1 2 3 0 4 9 11 8

[pair] verdict: host_joins=1/1 joiners_accepted=1/1 lost=0 dumps=0 -> PASS
```

So: three cities' data in play across the pair (Chicago the level, Havana on the
host, Rio on the client), **each player spawning in its own imported car** —
`InitPlayer` resolved `want=<model>` to `slot=5`, the slot carhacks imported into —
and the session live (mp's `adopt: player 1 snap …` counter climbing throughout).

The same script with `--host-both` asks the host for a second foreign city
(`import = 5:1:8, 6:3:9`) and shows the limit enforced loudly rather than silently:

```
host  [carhacks] import: slot 6 wants RIO but this level already reads cars from HAVANA -
      the engine holds ONE source city per level, so the entry is dropped
host  [carhacks] import set: level CHICAGO, guest city HAVANA, 1 entry, version 2
```

### Every player's car, on every machine

Three seats (host + two staggered joiners: `mp_pair.bat --players 3 --stagger 12`),
the players in different cars. Seat **c** - the second joiner, which never talks to
seat b, and whose own `PICK` would only ever have reached the host - ends up
knowing all three cars:

```
seat c  [carhacks/net] cars (from the host): 0=level model 1, 1=level model 3, 2=level model 3
seat b  [carhacks/net] cars (from the host): 0=level model 1, 1=level model 3, 2=level model 3
[pair] verdict: host_joins=2/2 joiners_accepted=2/2 dumps=0 -> PASS
```

Over the divergent pair the client ends up with the truth about a car it could
never derive locally (its own slot 5 holds Rio's car, the host drives Havana's):

```
client  [carhacks/net] cars (from the host): 0=HAVANA model 8, 1=RIO model 9
```

### Drawing a peer's car — measured

`chk_mp_foreign.sh` with `CHK_FORCE_PLAYER_PALETTE=2` (so the owner reports a
palette that is NOT the one this machine can honour). Each side imports its own
city into slot 5, so each draws the other as its own imported vehicle — and says
so, then corrects the colours:

```
seat a  peer 1 drives RIO model 9, but this machine draws HAVANA model 8 in slot 5
        - using that car's own colours (theirs is not loaded here)
        [mp] palette: player 1 reports 2, drawn as 0 (corrected: not their car here)
seat b  peer 0 drives HAVANA model 8, but this machine draws RIO model 9 in slot 5
        [mp] palette: player 0 reports 2, drawn as 0 (corrected: not their car here)
[pair] verdict: host_joins=1/1 joiners_accepted=1/1 dumps=0 -> PASS
```

One line per change, not per packet — the state stream is continuous. The fold that
would instead LOAD the peer's car is refused by the engine's one-city rule when the
cities differ (`import: slot N wants RIO but this level already reads cars from
HAVANA … dropped`), which is the honest limit, and `devcheck.sh` stays clean
(all 4 scenarios).

### Not verified

- **The hotload itself** (above): a peer's car from a city this machine does not
  hold is *corrected*, not loaded — right colours, wrong vehicle, and reported. The
  runtime load is the next unit, and the numbers it has to fit are recorded above.
- A session where the two machines' configs genuinely differ *and* the agreement is
  ON: mp's pair harness copies one `JERICHO/CONFIG` into both run dirs, so host
  authority is verified by construction and by the logs above, not by a
  divergent-config run. The three-city run deliberately turns the agreement off
  (`mp_agree_imports = 0`) for the same reason.
