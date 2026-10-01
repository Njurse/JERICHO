# Debug Orbit Camera (`debugorbit`)

Takes the camera over the moment a level starts and turns the subject into a
turntable, so a car — or Tanner on foot — can be inspected from every side:
both flanks, the wheels, the roof, and how the shadow falls.

**On by default** (`mod.toml`: `default-enabled = true`, and pinned to `1` in
`JERICHO/CONFIG/modlist.ini`). Turn it off in `modlist.ini` or in the frontend's
Options → JERICHO screen.

## What it does

| | |
|---|---|
| Hold | sits static for `hold_ms` (default 2.5 s) at a 3/4 front view — `start_angle` degrees off the subject's centreline, with the whole vehicle plus context in frame |
| Orbit | then turns around the subject at `speed` degrees per second (default 40 → a 9 s revolution) |
| Framing | a constant radius (`distance`, default 4200) and a constant `elevation` below horizontal (default 30°, i.e. the spec's 25–35° band), so the subject stays the same size and the same distance from the top of the frame throughout |
| Subject | whatever the engine's chase camera is chasing: the player's car, or Tanner on foot |
| Control | none — the pad is untouched. An idle car sits still and the orbit is a clean turntable; a driven one is simply watched |

It re-aims from scratch at every level start (fresh launch, restart, next
mission), so the hold is always the first thing you see.

## Config — `JERICHO/CONFIG/debugorbit.ini`

Written on first boot with the defaults visible, and **re-read live** (about
once a second), so re-tuning needs no rebuild and no restart:

| key | default | meaning |
|---|---|---|
| `enabled` | 1 | the kill switch; 0 hands the camera back to the engine |
| `distance` | 4200 | orbit radius, in the engine's world units (the stock chase camera sits ~850 behind a car) |
| `elevation` | 30 | degrees below horizontal, clamped 5..80. The height is derived as `distance * tan(elevation)` |
| `speed` | 40 | degrees per second, clamped ±360. Negative reverses the orbit |
| `hold_ms` | 2500 | the static hold before the orbit starts, milliseconds (the spec's 2–3 s) |
| `start_angle` | 45 | degrees from the subject's centreline: the 3/4 front view. Positive and negative put you front-right / front-left; 0 is straight behind |
| `orbit_dir` | 1 | which way the turn goes; negative reverses it (independent of `speed`'s sign) |

The `speed`/`orbit_dir` split exists because the revolution check in the log
reads `speed`'s sign as "how far we have turned": keep `speed` positive and flip
`orbit_dir` if you only want to change direction.

## How it takes the camera

`JER_EVENT_CAMERA` (`src_rebuild/Game/C/camera.c:195-216`), with
`args->override = 1`, so the engine rebuilds the view matrices from our
`camera_angle` and the `camera_position` we write is the one the renderer, the
frustum culling and the region streaming all use. Registered at **priority
1000** — lower priority runs first, so this is the last word on the camera each
frame and another module's chase-cam nudge cannot survive it.

The hook only fires while `events.cameraEvent == NULL`, so a scripted cutscene
camera still wins, and the orbit resumes when the cutscene ends. There is no
camera in the frontend at all (that hook is in-game only).

### Axis conventions (the part that is easy to get wrong)

* **y is inverted: a SMALLER y is HIGHER**, for the camera and the target alike.
  Measured, not derived: in one stock Chicago frame the car's `basePos[1]` was
  `114` while the engine's own chase camera sat at `y = -305` — and a chase
  camera looks slightly *down* at the car, so it is ~419 units **above** it at a
  **lower** y. A camera `height` above the subject is therefore
  `basePos[1] - height`. The first version of this module used
  `-basePos[1] + height` (the shape of `PlaceCameraFollowCar`'s own
  `cameraPos.vy = carheight - basePos[1]`, `camera.c:650`) and the camera spawned
  well under the road — the failure was invisible in the log and only showed in
  the frame.
* `basePos` is `LONGVECTOR4` `{x, y, z}` in the same units as
  `camera_position` (world units; the chase camera sits ~850 behind a car).
* The camera sits at `basePos + (sin(a), cos(a)) * distance`, exactly as the
  chase camera does (`camera.c:646-648`).
* `camera_angle.vy = -(a + 2048)` is the engine's own yaw for a camera placed at
  that offset (`camera.c:619`, and through `ratan2` at `camera.c:621`).
* `camera_angle.vx` is the pitch in PSX units, positive down (the chase
  camera's `camera_angle.vx = 25`, `camera.c:542`, and the raised-camera branch
  `(… >> 1) + 25`, `camera.c:583`).
* PSX angles are 4096 per turn. The logic frame is a fixed 30 Hz, so the orbit
  is computed from the tick count and cannot drift.

## SDK-only, on purpose

This addon imports **no** engine symbol: it calls only the context's function
pointers and mirrors the two engine structs it writes through (`VECTOR`,
`SVECTOR`) locally. A module DLL that imports engine symbols is bound to the exe
name baked into its import library (`REDRIVER2.exe` in the SDK copy,
`REDRIVER2_dev.exe` in the dev tree), so it only loads under whichever build that
library came from. Importing nothing keeps this addon loadable under any build —
which is also why it reads the pad nowhere.

It is therefore built with the SDK, not with the game:

```
JERICHO\sdk\build_mods.bat C:\path\to\REDRIVER2\JERICHO\MODS\debugorbit
```

## Limits

* The engine's audio listener (`player[].cameraPos`, `snd_cam_ang`) and the
  camera-collision box stay at the subject — the orbit is presentation only.
  Directional sound therefore does not swing with the camera. That is
  deliberate: nothing here can reach the handling model.
* Gameplay is not paused and no input is suppressed, so an analysis of a moving
  car shows the motion as it happens (which is usually what you want — and the
  reason to use `testmode` instead when the traffic has to be gone).

## Verifying a change

The module narrates itself, so a run can be checked without a debugger:

```
cd src_rebuild\bin\Release_dev
REDRIVER2_dev.exe -nointro -level chicago -car slot2 -weather none -time day -frames 200 -shot 120
REDRIVER2_dev.exe -nointro -level chicago -car slot2 -weather none -time day -frames 400 -shot 330
```

* `-frames N` exits cleanly after N gameplay frames; `-shot M` writes
  `SCREENSHOT.BMP` on frame M.
* `JERICHO.log` (or the captured stdout) carries the boot inventory line
  (`[jericho]   debugorbit v0.1.0  enabled=1 src=modlist state=active`), the
  options line, then a diagnostic line every ~1 s: the phase, the orbit yaw in
  degrees, our camera position/angle, the target, **and the engine's own stock
  camera position for that frame**. The stock value is the check on the y-sign
  convention: it is always *above* the subject (a chase camera looks slightly
  down at it) and above means a *smaller* y here, so if our camera y is on the
  other side of it the sign is wrong.
* A phase transition and the first completed revolution are logged explicitly —
  the check on the 8–12 s spec.

What a clean run looks like (measured, Chicago, `-car slot2`, 30 Hz):

```
[debugorbit] hold: subject heading 0 deg, camera at 45 deg (start_angle +45), 2500 ms, then 40 deg/s at radius 4200 and 30 deg down
[debugorbit] orbit: hold done after 76 frames, yaw starts at 46 deg
[debugorbit] t=29  hold  yaw=45  deg cam=(9186,-2311,-219512) ang=(341,1536,0) target=(6216,114,-222482) stockCam=(6216,-305,-223554)
[debugorbit] t=119 orbit yaw=103 deg cam=(10298,-2311,-223471) ang=(341,869,0)  target=(6216,114,-222482) stockCam=(6216,-305,-223554)
[debugorbit] t=179 orbit yaw=183 deg cam=(5952,-2311,-226674) ang=(341,4055,0) target=(6216,114,-222482) stockCam=(6216,-305,-223554)
```

Read it like this: `cam - target` is `(2970, -2425, 2970)` at yaw 45° —
`sqrt(2970² + 2970²) = 4200` (the radius) and `2425 = 4200 · tan 30°` (the
height, said the inverted way round: `ly = basePos[1] - 2425`). `ang.vx = 341` =
30° of pitch, `ang.vy` walks `1536 → 869 → 4055` as the yaw turns. The yaw
advances 40° per 30 frames (`45 → 103 → 183`), i.e. exactly 40°/s and a 9 s
revolution.
