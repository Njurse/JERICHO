# Test Mode (`testmode`)

A debug/test-suite mode for asset work: it makes the world quiet and puts the
camera on one thing, so a car or a Tanner can be inspected up close — which is
what texture and palette work needs (see `ped-palette.md`).

**Off unless enabled** (`JERICHO/CONFIG/modlist.ini`: `testmode = 1`).

## What it does

| | |
|---|---|
| Traffic | `maxCivCars = 0` — no civilian cars spawn (`main.c`, `civ_ai.c`) |
| Police | `CopsAllowed = 0` — no police spawn (`mission.c`, `cop_ai.c`) |
| Pedestrians | ambient peds are **despawned** each frame; there is no spawn cap to set, so this is a filter, not a gate (a ped spawned between frames can flash for one frame) |
| Camera | an orbit around the subject: Left/Right around it, Up/Down for distance and height, Circle resets the framing |

The subject is either the boot's car (`-car`, or `-testcar <n>` to pick a
different one) or a standing Tanner spawned a fixed distance in front of the
player's start position. The player's own car and ped are left alone: the sweep
removes only `CIVILIAN` peds, skips anything `jer_npc_owned()`, and skips a live
player ped by pointer regardless of its type.

## Flags

```
-testmode           arm the mode (subject: the boot's -car)
-testcar <slot|n>   arm it with that car as the subject
-testped            arm it with a standing Tanner as the subject
```

The engine's argument parser recognises the flags so they are not reported as
invalid arguments; the module reads the values itself via `JER_EVENT_CMDLINE`
(the same arrangement as the mp mod's `-host`/`-join`).

## Config — `JERICHO/CONFIG/testmode.ini`

Written on first boot with the defaults visible, so every knob can be edited
without a rebuild:

| key | default | meaning |
|---|---|---|
| `enabled` | 0 | same as passing `-testmode` |
| `subject` | 0 | 0 = car, 1 = Tanner |
| `car` | -1 | car slot/model for the subject; -1 = whatever `-car` selected |
| `ped_action` | 0 | which pose the subject Tanner is put in |
| `distance` | 4200 | orbit distance from the subject |
| `height` | 1200 | orbit height above it |
| `orbit` | 0 | orbit yaw in degrees (0 = in front) |

## Status

The module skeleton, its flags/config, and the **quiet world** are in: turning the
mode on removes traffic, police and ambient pedestrians, and logs a census so that
can be checked rather than assumed.

Still to come: the subject (the camera has nothing to point at yet) and the orbit
camera itself.

### The census

Every ~10s while the mode is on, the log carries the engine's own live counters
plus what the mode did to them:

```
[testmode] census: civcars=0 copcars=0 (maxCivCars=0 CopsAllowed=0) peds=1 pinged_cars=1 despawned_peds=587
[testmode]   survivor: padId=-1 owned=1 type=0 (tanner)
```

A survivor list follows, one line per pedestrian still alive (`padId`, whether
JERICHO owns it, and its `pedType`), because "nobody is left" is only meaningful
if you can see who *is* left: that is the player and anything a module owns.

**Why pedestrians are removed rather than blocked:** `maxCivCars` and
`CopsAllowed` are levers, but there is no equivalent for pedestrians - no
`maxPedestrians`, and the civilian spawner has no flag or cheat gate. So ambient
peds (`CIVILIAN`) are destroyed every frame instead, via
`DestroyPedestrian` (the engine's clean unlink, not a kill). It is a filter, not a
gate: a ped spawned between frames can appear for one frame.

One trap worth knowing if you touch this: **`padId >= 0` does not mean "the
player"** - the engine leaves `padId = 0` on civilian peds, so a filter written
that way keeps every civilian alive. The filter is `pedType`.
