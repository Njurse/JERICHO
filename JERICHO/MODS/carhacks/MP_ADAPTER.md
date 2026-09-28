# MP_ADAPTER.md — carhacks over the JERICHO net bridge

How carhacks and the multiplayer module (`JERICHO/MODS/mp/`) fit together: the car
**identity** schema, the **channel** carhacks multiplexes over the bridge mp
exposes, the host-authority rule, and the mp-side changes that are *proposed here
but deliberately not made*.

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
| size | ≤ 28 bytes payload (the bridge's cap is 1024) |
| with no session | every call is a no-op (`jer_net_is_active() == 0`) |

Message layout — `[wireVersion][tag]` then the payload:

```
REQ   (no payload)                     a joiner asks the host for the set
PICK  [city][model]                    "this is the car I want to drive"
SET   [guestCity][count][version]      the host's agreed import set
      count x [slot][city][model]
```

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

**The honest gap:** both use spare slots 5/6, and both answer
`JER_EVENT_CAR_DATA_SOURCE` at priority 0 (`mp.c:1422`, `carhacks.c`). They can
coexist because carhacks' fold only takes a spare slot that is *empty in the set*
and logs when none is free, and because the adapter is only active in a session —
but a match with several foreign cars and several extra players could collide. The
clean fix needs mp to tell carhacks which slots it reserved; that is an mp-side
change (see below), so it is recorded rather than worked around.

## Proposed mp-side deltas (NOT implemented here)

Kept deliberately out of scope — this work is carhacks-side only, so the mp module
and its wire format are untouched.

1. **Carry the city.** Add one byte to `MP_ROSTER_ENTRY` and
   `MP_CARSTATE_ENTRY` (`u8 modelCity`, `0xFF` = the session city) and bump
   `MP_PROTO_VERSION` (`mp_proto.h`), then resolve it in `MpPlayerCarModel`. That
   is the whole change needed for mp to describe a cross-city car natively, and it
   would make this channel unnecessary for identity (it would still be the way the
   *agreed guest city* is reached, since the engine holds one per level).
2. **Publish the reserved slots.** A small query so carhacks' fold can avoid slots
   mp has taken.
3. **Offer the roster in-session.** carhacks' menu declines while a session is live
   precisely because the stock car screen is mp's. Giving the roster row to mp's
   own car screen is the natural follow-up.

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

### Not verified

A session where the two machines' configs genuinely differ *and* the agreement is
ON: mp's pair harness copies one `JERICHO/CONFIG` into both run dirs, so host
authority is verified by construction and by the logs above, not by a
divergent-config run. The three-city run above deliberately turns the agreement
off (`mp_agree_imports = 0`) for the same reason.
