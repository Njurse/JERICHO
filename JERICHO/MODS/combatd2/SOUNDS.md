# Combat D2 — Sound bank reference (candidate weapon sounds)

How Driver 2's audio is laid out, the API a JERICHO module uses to play it,
and the sample indices worth trying for the weapon prototype. Everything here
is gathered from the engine source (`src_rebuild/Game/C/`); there is **no
symbolic sample-name table** in the code — samples are plain numeric indices
into a bank — so the tables below are the indices the game itself uses plus
their call-site meaning.

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

## Candidate weapon sounds

**Explosion (rocket impact) — already wired.** `weapons.c` calls
`AddExplosion(pos, LITTLE_BANG)`; `AddExplosion` (`job_fx.c:114`) calls
`ExplosionSound(pos, type)`, which plays `phrase_top` (SFX `3`/`7`) or the
mission explosion (`GetMissionSound(12/29)`). No extra code needed.

**Machine gun shot** — there is no dedicated gunshot in the bank, so audition
a short percussive SFX. First candidates: the crash samples (`SOUND_BANK_SFX`
`4`/`5`) pitched up a little. Wire it in `cd2MgFire`
(weapons/raycast/machinegun.c) at spawn time:

```c
// one line, world-positioned, auto channel
Start3DSoundVolPitch(-1, SOUND_BANK_SFX, 5,
    o.vx, o.vy, o.vz, -2000, 4096 + 2048);   // +2048 pitch = a bit snappier
```

**Rocket launch** — a rising "whoosh" is the closest match. Candidates:
- `SOUND_BANK_SFX` `12` (special siren, pitched way up) as a launch whistle, or
- a cop siren bank sample (`SBK_COP_SIREN_START = 69` onward) pitched up.

Wire it in `cd2MissileFire` (weapons/projectile/missile.c) with the muzzle
position. For a looping/tracking
sound that follows the projectile, use `Start3DTrackingSound(channel, bank,
sample, &rocket.pos, &rocket.vel)`.

## Auditioning a sample

There's no in-game sample browser yet; the quick way to try a candidate is to
temporarily play it from the pause-menu Debug toggle (or log its id), or —
during a playtest with **Modules → Combat D2 → Debug → Telemetry Log ON** —
watch `REDRIVER2.log` for the `[combatd2]` weapon lines (`weapons frame`,
`MG fired`, `draw world`) to correlate fire events with what you hear. Pick
`bank`/`sample`, set a `volume` near `-2000..-3000` and `pitch` around `4096`,
then tune up/down by ear.
