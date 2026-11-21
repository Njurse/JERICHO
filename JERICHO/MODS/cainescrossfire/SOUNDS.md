# Caine's Crossfire — Sound banks and weapon audio

The module's weapon, pickup and reticle sounds come from ONE compiled PSX
dynamic bank, `sounds/weapons.sbk`, generated from the WAVs in `sounds/` by
`tools/build_sound_bank.py`. The engine appends it to `SOUND_BANK_SFX` at level
load, so every sound plays through the normal SPU voice pool and the 3D helpers
(`Start3DSoundVolPitch` / `Start3DTrackingSound`) — position, doppler and
distance falloff included. A missing bank falls back to the weapon's previous
stock sample.

The rest of this file is the engine-side reference the module was built
against: the three audio layers, the play API, the voice budget and the stock
sample indices.

## Architecture (3 layers)

| File | Role |
|---|---|
| `Game/C/sound.c` / `sound.h` | Low-level: 16 SPU voice channels (`channels[16]`), bank loading, `StartSound` / `Start3DSoundVolPitch` / `Start3DTrackingSound`, per-channel volume/pitch, doppler. |
| `Game/C/gamesnd.c` / `gamesnd.h` | Game layer: the `SOUND_BANK_*` / `SoundBankIds` enums, `LoadBankFromLump`, `StartGameSounds`, `LoadLevelSFX`, and the higher helpers (`CollisionSound`, `ExplosionSound`, `CarHasSiren`, …). |
| `Game/C/mc_snd.c` / `envsound.c` | Mission/environment sound: `GetMissionSound(id)` (mission-bank sample mapping), doppler SFX, ambient `AddEnvSnd` city effects. |

Banks are loaded at boot (`StartGameSounds`) and per level (`LoadLevelSFX`)
from the data lumps (`LoadBankFromLump(bank, lumpId)`); the lump ids are the
`SoundBankIds` enum in `gamesnd.h`. The raw samples themselves live in the
game data (`DRIVER2/SOUND/MUSIC.BIN`, `VOICES2.BLK`, plus the packed SFX/VAB
lumps) — not in the repo.

## The play API

```c
// sound.h
int StartSound(int channel, int bank, int sample, int volume, int pitch);
int Start3DSoundVolPitch(int channel, int bank, int sample,
                         int x, int y, int z, int volume, int pitch);
int Start3DTrackingSound(int channel, int bank, int sample,
                         VECTOR *position, LONGVECTOR3 *velocity);
int GetFreeChannel(int force = 1);
```

- `channel` — `-1` to auto-allocate a free voice (`GetFreeChannel()`), or a
  fixed voice id (`0..15`, see `MAX_SFX_CHANNELS`).
- `bank` — one of the `SOUND_BANK_*` ids below.
- `sample` — numeric index into that bank.
- `volume` — **attenuation**: `0` = full/loudest, more negative = quieter,
  `-10000` = silent, positive = louder than full. Internally the engine
  computes `vol = MAX(0, 10000 + volume)` (`sound.c:UpdateVolumeAttributesS`).
  Existing in-game values: speech `-1500`, crash `-2750`, idle engine floor
  `-10000` (then scaled per-frame by the rev model).
- `pitch` — SPU pitch, `4096` = normal (the `CD2_SND_*` tuners use this too).
- 3D variants take a world position and attenuate by camera distance
  (`ComputeDoppler`), so they're the right choice for muzzle/impact/explosion
  sounds attached to a world point.

## Voices are scarce — reserve them (`jer_sound`)

There are only `MAX_SFX_CHANNELS` (16) SPU voices. The engine locks voices 0..2
for itself at boot (0..5 in two-player) and plays its own one-shot effects — the
**collision bang**, explosions, tyre screech — on whatever `GetFreeChannel()`
hands out. A module that holds a LOCKED voice per sound can leave the engine with
none: once every remaining voice is locked, `GetFreeChannel()` returns -1 and the
crash you just had makes no sound at all. That is exactly what happened here
before `jer_sound`: ~20 weapon/special sounds each locked a voice, so past ~13
collisions (and, eventually, everything else) went silent.

`jer_sound.h` fixes it, and the module routes every voice through it via
`cd2TakeVoice()` (`cainescrossfire.c`):

```c
int  jer_sound_lock(int channel);      // 1 = locked, 0 = REFUSED (reserve held)
int  jer_sound_ensure(int* channel);   // one-call acquire/re-lock/give-up
void jer_sound_unlock(int channel);
int  jer_sound_channel_busy(int channel);
int  jer_sound_locked_count(void);
```

- `jer_sound_lock` locks a voice **only while `JER_SFX_RESERVE` (4) voices stay
  free** for the engine, so a module can never starve the generic SFX pool.
- A REFUSED lock is not an error: `cd2TakeVoice` still returns a usable voice
  (`GetFreeChannel(1)` with FORCE never steals a locked voice), it just is not
  held. Play it; do not treat a refusal as "no sound".
- `jer_sound_ensure` is the wrapper for a cached `-1` channel variable: it
  acquires and locks, re-locks after a level change (`ResetSound` clears every
  lock), and gives up gracefully.
- With `debug_log` on, a run logs the budget: `sfx voice: chan=9 held=0
  locked=12/12` is the reserve being protected.

## The module's compiled weapon bank

`sounds/weapons.sbk` is a PSX dynamic bank: `int32 sample count`, that many
16-byte `(address, length, loop, rate)` entries, then the VAG payload. It is
the layout `LoadSoundBankDynamic()` reads and the same one the car bakes
write. The module requests it once at registration:

```c
sWpnSndReq = jer_sound_request_mod_bank("cainescrossfire", "weapons",
                                        SOUND_BANK_SFX);
```

The engine loads it in `LoadLevelSFX`, before the car-import memtop, and
records `jer_sound_mod_bank_base()` / `_count()`. The helpers re-read that base
on every play, so a level change cannot leave a stale index behind. A missing
file, a full bank or an omitted optional slot makes the helpers fall back to
the weapon's existing stock sample — the module never goes silent because of
the bank.

### Bank order

The order is the ABI between `tools/build_sound_bank.py` and
`cainescrossfire.h`'s `CD2_SND_*` enum (1-based; 0 = none):

| offset | id | source | loop | role |
|---:|---|---|---|---|
| 0 | MG_LOOP | machine_guns.wav | yes | MG/SMG held fire |
| 1 | MG_SHOT | machine_guns.wav | no | MG/SMG one-shot (AI + RT fallback) |
| 2 | FREEZE_FIRE | freeze_fire.wav | no | freeze launch |
| 3 | FREEZE_IMPACT | freeze_impact.wav | no | freeze impact |
| 4 | FREEZE_TRAVEL | freeze_ray_travel.wav | yes | freeze projectile travel |
| 5 | EXPLOSION1 | explosion1.wav | no | missile impact accent |
| 6 | EXPLOSION2 | explosion2.wav | no | seeker/zoomy impact accent |
| 7 | EXPLOSION3 | explosion3.wav | no | cluster parent impact accent |
| 8 | EXPLOSION4 | explosion4.wav | no | mine impact accent |
| 9 | PICKUP_WEAPON | pickup_weapon.wav | no | weapon pickup |
| 10 | PICKUP_HEALTH | pickup_health.wav | no | health pickup |
| 11 | RETICLE_SELECTED | reticle_selected.wav | no | Fixer lock acquired |
| 12 | RETICLE_CHARGE | reticle_charge.wav | yes | Fixer charge loop (pitch ramps) |
| 13 | RETICLE_FULL | reticle_full.wav | no | Fixer full charge / fire cue |
| 14 | SCREAM | scream.wav | no | weapon kill |
| 15 | SNIPER_SHOT | gun_sniper_rifle_shot.wav | no | optional sniper shot |

`meldedv8.wav` is deliberately NOT in this bank: it is an engine loop for the
vehicle bank. `gun-sniper-rifle-shot.mp3` is not PCM; the builder converts it
with ffmpeg when available and otherwise leaves slot 15 out (the code checks
the loaded count, so the other indices do not shift).

### How a weapon uses it

`CD2_WEAPON_DEF` carries six optional fields:

```c
int fireSound;    // CD2_SND_* one-shot at the muzzle (0 = stock)
int firePitch;    // SPU pitch, 4096 = normal (0 = default)
int impactSound;  // CD2_SND_* accent at the impact (0 = none)
int impactPitch;
int travelSound;  // CD2_SND_* LOOP while a projectile flies (0 = none)
int travelPitch;
```

The helpers in `weapons/core/weapons.c` resolve these against the live bank:

- `cd2WpnSndFire(def, &channel, x, y, z, fallbackSample, fallbackVolume,
  fallbackPitch)` — one-shot at the muzzle. Every weapon fire path goes through
  it; the MG/SMG player loops are the exception described below.
- `cd2WpnSndImpact(...)` — one-shot at the impact. It LAYERS over the engine's
  own explosion sound; the stock explosion is never suppressed.
- `cd2WpnSndTravelStart/Stop` — a 3D tracked loop owned by the projectile pool
  (`projectile.c` adds `sndChan` to each shot). At most **4** travel loops run
  at once (`CD2_WPN_TRAVEL_MAX`), so a volley cannot eat the 16-voice pool.
- `cd2WpnSndPlay` / `cd2WpnSndPlay2D` — a one-shot by `CD2_SND_*` id, 3D or
  non-positional.

### MG, SMG and the held-fire loop

Firing the MG every 2 frames (15/s) cannot retrigger a 1.3 s burst sample. The
player's MG therefore starts `MG_LOOP` ONCE on a 3D-tracked channel when LT is
held and stops it on release. The SMG reuses the same loop for its 14-frame
six-shot burst at pitch **5120** (slightly above the MG's 4096). AI cars and
the RT fallback never loop: they play the one-shot `MG_SHOT`. The loop channels
use `jer_sound_ensure`, so they re-lock after a level change; `cd2WpnSndStopAll`
stops them on disable, no player car, totaled/frozen/editor, reset and the
frontend.

### Fixer's reticle cues

Fixer's Laser Lock is the one charging special. For the LOCAL player only:

- `RETICLE_SELECTED` plays when a new lock is acquired;
- `RETICLE_CHARGE` loops while the charge builds, with `SetChannelPitch`
  ramping from 4096 to 8192 across `CD2_FIXER_CHARGE` (5.5 s);
- `RETICLE_FULL` plays once at full charge, the "fire now" cue;
- a world-space reticle (four corner brackets) is drawn on the locked car in
  the beam's white → yellow → red ramp and pulses at full;
- losing the target, firing, or SWAPPING WEAPONS stops both cues and clears the
  lock — weapon swap is the player's abort.

AI Fixers keep the beam but make no reticle noise; the top-right lock-on name
and health panel is a separate, wider lock and is not cleared by this.

## Banks (`gamesnd.h`)

```c
#define SOUND_BANK_DUMMY        0
#define SOUND_BANK_SFX          1   // general effects (crashes, explosion, siren)
#define SOUND_BANK_VOICES       2   // speech ("phrases")
#define SOUND_BANK_CARS         3   // per-car engine + horn + siren
#define SOUND_BANK_ENVIRONMENT  4   // city ambience (AddEnvSnd)
#define SOUND_BANK_MISSION      5   // per-mission SFX/speech (GetMissionSound)
#define SOUND_BANK_TANNER       6   // Tanner-specific SFX
```

The `SoundBankIds` enum (`gamesnd.h:13+`) is the *lump* id list passed to
`LoadBankFromLump` (e.g. `SBK_ID_SFX = 1`, `SBK_ID_MISSION_2 = 45`,
`SBK_ID_HAVANA_TAKEADRIVE = 64`, `SBK_COP_SIREN_START = 69`, …).

## Known sample indices (what the game already plays)

### `SOUND_BANK_SFX` (general effects)
| Sample | Meaning | Where |
|---|---|---|
| `4` / `5` / `6` | crash/impact (increasing severity) | `CollisionSound` `gamesnd.c:1544` |
| `12` | special-car siren | `CarHasSiren` `gamesnd.c:182-192` |
| `phrase_top` (`3` or `7`, level-dependent) | generic explosion | `ExplosionSound` `gamesnd.c:1610`, set in `LoadLevelSFX` `gamesnd.c:402-412` |

### `SOUND_BANK_MISSION` (`GetMissionSound(id)`, `mc_snd.c:130`)
`id_map[]` + `missionstarts[]` map a logical id to a per-mission sample
offset; the id→use is only documented by call-site:
- `12`, `29` — explosion variants (`ExplosionSound` `gamesnd.c:1602-1606`)
- `11` — positional mission effect (`mc_snd.c:469` etc.)
- `14`, `24`, `26`, `31`, `36`, `39` — mission/cutscene effects & alarms
  (`mc_snd.c:320-519`)
- `20` — mission speech (`mc_snd.c:503`)

### `SOUND_BANK_CARS` (per-car)
`GetCarBankSample(model)` returns a car sample id; the engine plays
`id*3` = engine loop and `id*3 + 1` = horn (`gamesnd.c:563-567`); sirens come
from `CarHasSiren()` returning a packed `(bank << 8) | sample`.

### `SOUND_BANK_TANNER`
`4` — a Tanner SFX used in cutscenes (`gamesnd.c:1727`, `mc_snd.c:206`).

## Per-weapon mapping (current)

| weapon | fire | travel | impact |
|---|---|---|---|
| MG | MG_LOOP (held) / MG_SHOT | - | - |
| SMG | MG_LOOP at 5120 / MG_SHOT | - | - |
| FREEZE | FREEZE_FIRE | FREEZE_TRAVEL | FREEZE_IMPACT |
| MISSILE | stock 6 (config `missile_sound`) | - | EXPLOSION1 |
| SEEKER | stock 5 | - | EXPLOSION2 |
| CLUSTER | stock 6 | - | EXPLOSION3 |
| ZOOMY | stock 5 | - | EXPLOSION2 |
| MINE | stock 5 | - | EXPLOSION4 |
| SHOTGUN | MG_SHOT pitched down | - | - |
| FIXER | SNIPER_SHOT (if converted) | - | - |
| other specials | stock sample + pitch | - | - |
| pickups | PICKUP_WEAPON / PICKUP_HEALTH | - | - |
| weapon kill | SCREAM | - | - |

The explosion assignments are the starting defaults; every field is data, so
retuning one is a one-line change in that weapon's `cd2Make*Def()`.

## Reviewing and regenerating the bank

The builder lives at `tools/build_sound_bank.py` and uses the bake suite's
`vag.py` (the same encoder the car banks use):

```
python tools/build_sound_bank.py --review   # format/level/loop-seam report
python tools/build_sound_bank.py --build    # write sounds/weapons.sbk
python tools/build_sound_bank.py --check    # decode it and verify flags/rates
python tools/build_sound_bank.py --all      # build then check
```

`tools/build_sound_bank.bat` is the same commands on Windows. `--review`
reports rate/channels/duration/peak/RMS/crest/DC/clipping for every source and
the best block-aligned loop for `machine_guns`, `freeze_ray_travel` and
`reticle_charge`; it does not audition them. The builder levels each sample to
retail's car-bank loudness (peak 0.999 / rms 0.34), strips DC, fades one-shots,
keeps the source rate, and writes the three loop samples with VAG loop flags.

The engine-side bank format and loading rules are in `jer_sound.h`
(`jer_sound_request_mod_bank`) and `src_rebuild/Game/C/sound.c`
(`LoadSoundBankDynamic`).
