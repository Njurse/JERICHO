# Director Box (`directorbox`)

A camera-only addon that turns the game into a director's cut of whatever the
player is doing. It is switched on with one row in the in-game pause menu and
then cuts between six static framings, two moving shots (a slow orbit and a
rising crane), and an occasional random roadside tripod — all focused on the
player's car or Tanner on foot.

**Off by default.** Enable it once in **Options → JERICHO**, then use
**Pause → JERICHO Addons → DIRECTOR BOX → Director camera: ON/OFF**. Switching
it ON closes the pause immediately, so the first shot is visible at once.
Switching it OFF hands the camera straight back to the engine.

## What it does

| | |
|---|---|
| Cuts | a hard cut to the next shot every `shot_ms` (default 5000 ms) |
| Cycle | a shuffled bag of **nine** shots per pass: six static framings, two moving shots, and one random tripod |
| Subject | whatever the engine's chase camera is chasing: the player's car, or Tanner on foot |
| Static framings | chase, front 3/4, side, low close-up, high wide, tail |
| Moving shots | a 180° orbit around the subject, and a rising crane that pulls back from behind to the front quarter |
| Tripod | a fixed world vantage sampled around the subject; the camera stays put while its aim pans with the subject |
| Control | none — the pad is untouched, gameplay is not paused, and no input is suppressed |

The bag is reshuffled after the last cut, so every pass contains all nine shots,
in a different order each time. The tripod's vantage is sampled afresh every
time it comes up. On foot, the shot distances and heights are scaled to 70% so
Tanner fills the frame like a car does.

### Static shot table

Azimuth is relative to the subject's heading: **0° is directly behind**, 180°
directly ahead, ±90° at the flanks.

| shot | azimuth | distance | height | character |
|---|---:|---:|---:|---|
| `CHASE` | 0° | 1150 | 400 | behind and slightly above |
| `FRONT 3/4` | 150° | 1050 | 300 | ahead-right, looking back |
| `SIDE` | 90° | 1350 | 220 | flank tracking |
| `LOW` | 35° | 650 | 180 | close, low rear-quarter (raised clear of the road) |
| `HIGH WIDE` | 200° | 2100 | 1500 | elevated front-quarter, wide |
| `TAIL` | −30° | 950 | 200 | just off the rear-left corner |

### Moving shots

These interpolate their placement over the shot while the aim stays locked on
the subject:

| shot | movement | character |
|---|---|---|
| `ORBIT` | 60° → −120° (180° sweep), radius 1450, height 450 | circles from the rear quarter round to the front quarter |
| `CRANE` | 0° → 160°, distance 950 → 2300, height 180 → 1400, smoothstep | starts low and close behind, then rises and pulls back to a high front quarter |

Both are moving cameras in world space, not cuts between two static framings;
the car stays in frame because the aim is recomputed every frame.

### Decoupled angles

At every cut there is a **1-in-3** chance the shot's offset is captured in
**world space** instead of following the car's live heading. The camera then
holds that static angle while the car rotates underneath it; the aim still
tracks the car, so it stays framed. This applies to the static and moving
shots; the tripod is world-anchored by definition. The log reports `basis=car`
or `basis=world`.

### Tripod sampling

When the bag reaches the tripod slot, the module samples up to **24** candidate
vantages in the subject's current map region:

* random bearing in the **120°–240°** arc (ahead/side of the subject), so the
  shot usually sees the subject approaching or passing;
* random distance **1200–2200** units and height **550–700** above the ground;
* `map_height` must return a loaded ground plane (rejecting void / unstreamed
  areas);
* `cell_empty` with a camera-sized radius (**80**) must pass, keeping the
  tripod out of buildings and scenery.

If none of the same-region candidates pass, it retries **16** more without the
region constraint. If the tripod still cannot be placed, the shot falls back to
`CHASE` and the reason is logged.

The accepted vantage is fixed for the whole shot. Its `y` is derived from the
sampled ground as `−ground − height`, and only the aim is recomputed each frame,
so the camera pans like a tripod while the subject moves through the frame.

### Ground clamp

Every shot — static, moving or tripod — is then clamped at least **150** units
above the ground under the lens (`map_height`). A shot's height is relative to
the subject, not to the terrain under the camera, so without this a shallow shot
over a bank or a raised junction can sit under the local surface even when its
subject-relative height is correct.

## Config — `JERICHO/CONFIG/directorbox.ini`

Written on first boot. The pause-menu row writes these values through
immediately; a hand-edited ini is read at the next game launch (the host
config cache is per-run):

| key | default | meaning |
|---|---|---|
| `enabled` | 0 | the camera switch; the pause-menu row writes this |
| `shot_ms` | 5000 | milliseconds per shot, clamped 5000–60000 |
| `tripod` | 1 | 0 removes the tripod slot from the bag (leaves the six static and two moving shots) |

## Axis conventions

These are the engine's own, and the part that is easy to get wrong:

* **`y` is inverted: a smaller `y` is higher** for the camera and the target
  alike. A camera `height` above the subject is therefore
  `basePos[1] − height`.
* The camera sits at `basePos + (sin(a), cos(a)) × distance`, where `a` is the
  subject's heading plus the shot's azimuth, exactly as the chase camera places
  itself (`camera.c`).
* The engine's yaw is `camera_angle.vy = −(a + 2048)` in PSX units; the pitch is
  `camera_angle.vx`, positive looking down, computed from
  `atan2(above, horizontalDistance)`.
* PSX angles are 4096 per turn. The game steps at a fixed 30 Hz, so the cut
  timer is a tick count and cannot drift.

## How it takes the camera

`JER_EVENT_CAMERA`, registered at **priority 1000** (lower runs first), so it is
the last word on the camera each frame. It writes the engine's
`camera_position` / `camera_angle` through the event arguments and sets
`override = 1`, which makes the engine rebuild the view matrices from our
values. The hook only fires while no scripted cutscene camera is active, so a
cutscene still wins and the director camera resumes afterwards.

The module is **SDK-only**: it imports no engine symbol, uses
`jerHostApiGet()` for config and the pause menu, mirrors the two engine structs
it writes through (`VECTOR`, `SVECTOR`) locally, and reaches `map_height` /
`cell_empty` through the host API's car table. That keeps the DLL loadable under
`JERICHO.exe` and `JERICHO_dev.exe` alike.

## Limits

* The engine's audio listener and camera-collision box stay at the subject, so
  directional sound does not swing with the camera. This is presentation only.
* Shots do not test line of sight; the ground clamp keeps every camera above
  the terrain, but a very tight alley can still put a wide shot inside a wall.
  The tripod shot additionally uses `cell_empty`, and all shots can be shortened
  or reordered with the config.
* There is no lens/FOV zoom: the SDK exposes no projection-distance control.
  Framing is done with distance and height.
* The module is a normal runtime addon; it needs no game rebuild.

## Build and verify

```
JERICHO\sdk\build_mods.bat JERICHO\MODS\directorbox
```

or run `build.bat` from this folder, which also mirrors the DLL into the runtime
`src_rebuild\bin\Release_dev\JERICHO\MODS\directorbox`.

A headless smoke run:

```
cd src_rebuild\bin\Release_dev
JERICHO_dev.exe -nointro -level chicago -car slot2 -weather none -time day -frames 500 -shot 100
```

`JERICHO.log` should show the boot inventory line, the options line, a cut line
for each shot (including `ORBIT`, `CRANE` and a `TRIPOD` line with its
vantage/ground/attempts), and a periodic camera/target diagnostic carrying
`gnd=` (the ground under the lens), `clamp=` (how far the camera was raised to
clear it), `basis=car|world` (whether the shot follows the car's heading or a
heading frozen at the cut) and `dir=` (the car's live heading in degrees — under
a `basis=world` shot it changes while the camera's `ang` stays put). Turning
`enabled` off logs `stock camera restored` and stops the camera writes.
