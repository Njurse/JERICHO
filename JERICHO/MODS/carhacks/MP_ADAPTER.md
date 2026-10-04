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

On top of that, while the *geometry* side is now per-city (`gCarImports[4]`,
one block per slot), the palette side is one bank of **three guest-city blocks**
(`civ_clut` rows 8..31, one 8-row block each) — and the import SET still admits
one foreign source city by default, so a session still has to *agree* which city
that is. Today it is whatever each machine's own `carhacks.ini` happens to say.

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

**The set holds as many cities as it names.** `chkNetFoldPeerCars()` folds every
peer's car into the level's import set, so a peer is drawn as their own vehicle.
The geometry side holds several cities at once (`gCarImports[4]`, one source city
per slot) and the palette bank holds three (`civ_clut` rows 8..31) - and the set
itself now names as many source cities as it has slots: the one-guest-city gate is
gone. A car from the level's own city is skipped - it needs no import, and
skipping it keeps an identity that has not settled yet from claiming a spare slot
with the level's own model 0.

**Why that is not the whole fix.** A level reads its car files ONCE
(`JER_EVENT_CAR_DATA_SOURCE` → `ProcessCarModelLump`), and a joiner's car is only
known after it joins - so on the machine that loaded first, a peer's car can never
be in the set in time, whatever city it is from; loading it afterwards is `JerHotLoadCarModel`,
`JerHotLoadCarCosmetics` and `JerHotLoadCarTpages` (all `✓` below).

### The resource lifecycle: what each join/leave event must load and unload

Every resource a carhacks import holds is attached to a RESIDENT SLOT (the manifest above
says which), so every session event is really a question about slots. This is the contract
to monitor; `CarSlotResReport()` prints it at the moment the session ends, and the
`cross-city: slot N holds ...` lines print it whenever the pin walk finishes.

Both columns exist now. The release side is `JerReleaseCarSlot(slot)` - the pins and their
lower-half pool pages, the baked 110..127 index each of the slot's sets holds (and the
reservation that keeps it out of the next import), the geometry block, the manifest entry -
plus the module's half (`chkImportReleaseSlot`), which clears the set entry, the slot's
entry in `residentCarModels[]` and its source city. Leave either half and the slot still
looks taken (`chkImportSlotFree` checks all of it), so the next joiner is refused a spare
that is really free.

| event | what must LOAD | what must UNLOAD | today |
|---|---|---|---|
| **local player picks** (before the level) | the pick's city data (`InitCarImport`), its palette block, its page lists; the pick's slot is canonical | the PREVIOUS pick's slot, if it was a different car | load ✓, unload ✓ (`chkImportApplyPick` releases the old slot) |
| **local player changes car mid-match** | the new city's data; a hot load into a slot (geometry, cosmetics, pages, rows) | the old slot's resources | load ✓, unload ✓ (same path) |
| **peer joins, before the match starts** | the peer's city at the next level load (through the set) | nothing | ✓ |
| **peer joins, match in progress** (the catch-up) | the peer's city read mid-level + a hot load into a slot | nothing (a new slot) | ✓ |
| **peer leaves** | nothing | that peer's slot | ✓ (`chkNetReleaseDeparted`: engine + module; the car table shows a departure, and the roster catches it when the table cannot - the HOST's own case, `jer_net_player_present`) |
| **local player leaves the session** (back to the frontend) | nothing | EVERYTHING cross-city: the set, the picks, the pins, the pool pages, the blocks, the page lists, the deferred palette lumps | ✓ (`chkImportReleaseAll` -> `JerReleaseAllCrossCity` + `JerReleaseCarImport`) |
| **a new level loads** (restart, city change, rejoin) | the whole set from scratch | the previous level's state | ✓ (`InitCarImport` -> `CarImportResetState`) |

The lifecycle rule the table encodes:

  a slot's resources are the LOAD of the event that created it, and are given back by the
  UNLOAD of the event that removed it - either that peer leaving, that pick being replaced,
  the local player leaving the session, or the level going away.

The monitor criteria, in the same terms: after a leave, `CarSlotResReport()` must show
**no slot holding pins or geometry that no player is driving**; a slot still listed is a
leak, and the count should return to what it was before the peer joined. `CarSlotResReport()`
runs after the release now, so a clean leave prints nothing after it.

### The hotload (DONE: geometry, cosmetics, pages, catch-up)

A car folded in after the level loaded gets **all four** things a level-load import gets,
and a joiner is given them too:

| what | how |
|---|---|
| geometry | `JerHotLoadCarModel(slot)` (`models.c`) - the same three `GetCarModel` calls the per-slot build makes, into a pool this module owns (it cannot use `malloctab`: that is the level's heap, rewound per load). Refuses rather than half-building. |
| source city | `JerSetCarModelSource(slot, city)` (`mission.c`) - the per-slot source array was writable only from `JER_EVENT_CAR_DATA_SOURCE`, before the models are built. |
| cosmetics | `JerHotLoadCarCosmetics(slot)` (`cosmetic.c`) - `car_cosmetics[slot]` + `FixCarCos`, which is where the WHEELS (`wheelSize`/`wheelDisp`), the SHADOW corners (`cPoints`), the collision box and the COG come from. Without it a hot-loaded car wore the host LEVEL's wheels and handling. |
| texture pages + rows | `JerHotLoadCarTpages()` (`texture.c`) - re-runs the level-load walk (`LoadImportedTPages`), which is what records a car's pages (`CarPinRecord`); `CarImportPin` then places them and uploads their CLUT rows. Without it the model's polys read the local city's pages: "the cosmetic file loaded but the model retained the local city's materials". `CarPinRecord` is idempotent now, so a re-walk leaves the cars already pinned alone. |
| the rebuild | mp's swap (`cp->ap.carCos = &car_cosmetics[slot]` + `CreateDentableCar`) for a car already on the road, logged. A hot-loaded car's cosmetics are in place before it can resolve the slot, so the rebuild takes the right ones. |

The trigger is carhacks' `chkImportHotLoad(slot)`, called from the fold **and** - the
catch-up - from the two places a joiner learns who drives what: the `CHK_NET_CARS`
handler (the per-player table) and `chkNetApplyAgreedSet` (the host's set). A joiner's
level loaded before any of those cars existed in it, which is what joining a match in
progress means; without the catch-up it drew them as the level's own car of that number
("player 2 saw the imported Vegas truck, but player 3 didn't see player 2's car").

The slot each car takes is canonical (`chkImportCanonicalSlot`, `carimport.c`): the
lowest owning player id first, taking the i-th spare the LEVEL leaves free. Ordering by
player id is append-only, so a joiner cannot displace a car already in a slot, and the
same `(city, model) -> slot` holds on every machine - which is what makes the pages and
rows that were baked against that slot agree.

**Still open, and they are palette/VRAM placement rather than the load:**
- On the HOST, a second imported city's textures and colours come out wrong while the
  first is right ("the vegas car imported proper but not the havana one's textures and
  colors"). Two guest cities' pages and CLUT rows are placed in the same pool and bank,
  so this is a collision between guests, not a missing import.
- The machine that imported a car gets its SCENERY/building textures contaminated, and
  its car palettes were "still messed up" until the band fix. The contamination is the
  pin's own wording - "paged in at draw time, **evicting the world if needed**" - and an
  import has no business taking a world page: the lower-half pool is where it belongs.
  Worth testing with **a different palette number per test player** (within the vehicle's
  own palette count) so a mix-up is unambiguous.
- **CLOSED: an access violation when a player leaves a session and rejoins picking a car
  from a DIFFERENT city than before.** Returning to the frontend means there is no map, and
the cross-city state used to be reset only when a level LOADED (`InitCarImport` ->
`CarImportResetState`), so the frontend ran with the last map's cross-city state in place:
the deferred palette lumps (`sImpPalLump[]`/`sImpPalSize[]`) and the parsed page lists
(`gCarImportPerms[]`/`gCarImportSpecs[]`/`gCarImportTexParsed[]`) are pointers INTO the
import buffers and were only cleared by the next level's load, and the import buffers
themselves were freed there too. The fix is the cleanup the report asked for:
`chkImportReleaseAll()` on the session ending -> `JerReleaseAllCrossCity()` (every slot,
then `CarImportResetState`, `CarImportPaletteReset`, the page lists) and
`JerReleaseCarImport()` (the buffers and the hot-load pool). Verified with `MP_TEST_LEAVE`:
the leaver's own log shows the session ending, every slot released, the buffers freed and
`released everything - no map is loaded`, with no dump.
- The dented variant's textures come out "a little messed up" on an imported car: the
  damaged model is built (`gCarDamModelPtr`) but its page/row needs are the same walk's,
  so a damaged-only set is the place to look.
- The palette bank holds three guest cities (`CIV_CLUT_ROWS 32` / `CIV_CLUT_IMPORT_ROW 8`
  / `CIV_CLUT_BLOCK_ROWS 8`), and the tpage remap indices are `110..127` (18) - both are
  real ceilings, and both are where a multi-city session runs out first.

**Kept as the record of the measurement that started this.** It was taken BEFORE
the per-city import block landed, when a second city's geometry really was absent,
and with the one-source-city gate opened by hand (a measurement lever that has
since been deleted along with the gate itself) so the ENGINE finally saw a set
naming two cities:

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

The per-slot source city is real, and each slot now builds from its own city
(`ProcessCarModelLump`, `models.c:764-783`). That is what the hotload actually
has to do:

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

The **host is authoritative**, so every machine draws the same cars. That is no
longer a limit being worked around: a set may name as many source cities as it has
slots (see `chkImportSetSlot`), and the host's set simply wins. `mp_agree_imports = 0` (`carhacks.ini`) turns the whole agreement off —
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

## mp-side deltas — all three are DONE

1. **Carry the city. DONE** (`MP_PROTO_VERSION` 6). `MP_ROSTER_ENTRY.modelCity` and
   `MP_CARSTATE_ENTRY.modelCity` carry the city (`0xFF` = the session city), and the
   carstate's `model` is the model NUMBER, not a slot. `MpResidentSlotForCar` resolves
   the pair to the receiver's OWN slot, and `-mpcar [city:]model` names one.
2. **Reserved slots. DONE**, by observation rather than a query: carhacks answers the
   source event after mp and skips slots the engine already holds a model in (above).
   It now also skips them when the host PUBLISHES its set (a peer's PICK, a session
   start), which is outside that event: the resident list is COPIED out of the hook and
   answered from the copy, because the publish-time fold used to take mp's slot 5.
3. **Offer the roster in-session. DONE** (`carselect.c`). The menu arms inside a live
   session (the single-player gates apply only outside one) and `Ride` fires
   `JER_EVENT_MP_FRONTEND`/`JER_MP_FE_START` — the event the stock frontend raises on
   `BTN_START_GAME` — so **mp** launches and the pick travels in `wantedCar[0]` as
   before. See `CARSELECT.md`. Still open: a peer whose car this machine cannot build
   is corrected rather than loaded (the hotload below); the set is applied once per
   level, so it takes effect at the next load.

### An own-city car the level does not pool

`chkImportApplyPick` and `chkNetFoldPeerCars` used to skip a car from the level's own
city on the assumption that "the level already lists its own city's vehicles". A level
pools only the models its OWN list names — Rio's model 12 (the special) is in Rio's
files and in no Rio take-a-ride level — so the host was left with nothing to build the
peer's car from and `InitPlayer` fell back to resident slot 0: the level's FIRST car,
which is "I picked car 12 and spawned as car 1". Both callers now ask
`chkImportLevelHoldsModel` and import the car from its own city's files when the pool
provably does not hold it.

That helper answers in THREE values (held / provably not / cannot tell), and the third
is load-bearing: the fold also runs from the publish path outside the hook. Reading
"cannot tell" as "not held" there folded an own-city car the level already had, and an
own-city "guest" writes the level's own city's car palettes into a guest `civ_clut`
block — every colour in the session goes wrong. Callers that would import on a "no"
treat "cannot tell" as "leave it alone".

### The spare-slot pool

`CHK_IMPORT_MAX_SLOTS` is 11 (resident slots 0..10; 11 is the engine's
`SPECIAL_CAR_SLOT`), not 8: with the old cap the search had 5, 6, 7, mp claims two of
those for extra players, and a second cross-city pick was refused with "no spare
resident slot is free". Each distinct source city still needs a palette block and the
bank holds three (`civ_clut` rows 8..31), which is the real limit on how many CITIES a
session can mix — see `docs/VRAM.md`.

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
registration — the local cross-city path is unchanged (`chk_suite.sh`'s stock control
and imported-player rows stay clean).

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
(`import = 5:1:8, 6:3:9`). That used to be REFUSED loudly - the entry dropped and
only one car imported:

```
host  [carhacks] import: slot 6 wants RIO but this level already reads cars from HAVANA -
      the engine holds ONE source city per level, so the entry is dropped
host  [carhacks] import set: level CHICAGO, guest city HAVANA, 1 entry, version 2
```

With the gate gone both load. Measured 2026-10-03 on CHICAGO: a 5-entry set naming
HAVANA, VEGAS and RIO reported `import set: level CHICAGO, guest cities 3, 5
entries`, imported all 5, and each slot built its geometry from its own city.

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

One line per change, not per packet — the state stream is continuous. The fold adds
the peer's car to the set like any other entry, from whatever city it belongs to
(`chkNetFoldPeerCars`), and `chk_suite.sh`'s single-import rows stay clean.

### Not verified

- **The hotload itself** (above): a peer's car from a city this machine does not
  hold is *corrected*, not loaded — right colours, wrong vehicle, and reported. The
  runtime load is the next unit, and the numbers it has to fit are recorded above.
- A session where the two machines' configs genuinely differ *and* the agreement is
  ON: mp's pair harness copies one `JERICHO/CONFIG` into both run dirs, so host
  authority is verified by construction and by the logs above, not by a
  divergent-config run. The three-city run deliberately turns the agreement off
  (`mp_agree_imports = 0`) for the same reason.

## The live-car request — `mplive.{c,h}`, and mp's pause menu

mp's Multiplayer pause page has a `Change car` picker (city, car, apply). The
picker is mp's, but two of the things it needs are carhacks', so mp ASKS for them
over two custom JERICHO events — the contract is
`JERICHO/MODS/mp/mp_carquery.h`, the one place the ids and argument structs are
written down, and `mplive.c` is the carhacks side:

| Event | mp asks | carhacks answers |
| --- | --- | --- |
| `MP_CARQ_CITIES` | which cities can this session offer? | the 0..3 indices, or `count = 0` for "nobody knows" (the cross-city hack is off, so this machine has only the level's own city) |
| `MP_CARQ_LOAD` | make `(city, model)` available here and in the session | `ok = 1` once the slot is set, built and the session told |

`MP_CARQ_LOAD` is not new machinery. It is the sequence a mid-match peer pick
already goes through: `chkImportSlotForCar` (a car already in the set keeps its
slot — re-slotting it would move a car somebody may be driving), else
`chkImportCanonicalSlot` for OUR player id, then `chkImportSetSlot` +
`chkImportHotLoad` (which writes the slot's identity into the engine's arrays,
reads that city in, builds the slot's geometry in the engine's own pool, applies
its cosmetics and records its texture pages), then `chkNetAdvertisePick` and, on
the host, `chkNetPublishSet` so every other machine loads it too.

`ok` is only 1 when `gCarCleanModelPtr[slot]` is non-NULL after that: mp is about
to point a car at this slot, and a slot with no built geometry is a crash, not a
cosmetic glitch.

Measured on the pair rig (`MP_TEST_PAUSECAR=40,1`, which runs the same call the
Apply row does — HAVANA model 2 on a RIO map): the host logs `rebuilt player 0's
car on slot 7 (model 2 from HAVANA)`, `told the session: HAVANA model 2`, and the
client logs `cars (from the host): 0=HAVANA model 2` with HAVANA's palettes
uploaded for its block. So the runtime import path above is now exercised by a
live pick, not only by the level-load walk.

The one thing this does NOT do is free the slot when a player picks away from a
car: `chkImportReleaseSlot` on a live re-pick is still the open half of the
lifecycle (see the "Slot ownership" section).
