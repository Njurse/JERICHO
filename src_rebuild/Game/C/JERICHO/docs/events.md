# JERICHO events

Argument structs live in `src_rebuild/Game/C/jer_events.h` (game-side; the
SDK stays generic). Engine call sites are tagged `// JERICHO-HOOK` and are
inert no-ops when no module handles them.

| Event | Args | Fired from | Meaning |
|---|---|---|---|
| `JER_EVENT_BOOT` | — | `jer_init` | once, after all modules activated |
| `JER_EVENT_FRAME` | — | `GlobalTimeStep` | every game frame |
| `JER_EVENT_PRE_SIM` | — | top of `StepSim`, before the car-control loop reads the pads | inject/replace pad input, or block for external input (wait-for-input lockstep) |
| `JER_EVENT_INIT` | — | `InitialiseDenting`, game-load | damage system (re)init |
| `JER_EVENT_COLLISION` | `JER_ARGS_COLLISION` | car-car (`handling.c`) + car-world (`bcollide.c`) | record an impact |
| `JER_EVENT_DENT_PASS` | `JER_ARGS_DENT_PASS` | deferred denting pass (`denting.c`) | deform car verts |
| `JER_EVENT_RESET_CAR` | `JER_ARGS_RESET_CAR` | `denting.c` | clear a car's damage state |
| `JER_EVENT_DEBUG_TICK` | — | `GlobalTimeStep` | per-frame debug/tuning |
| `JER_EVENT_CAR_AVAILABILITY` | `JER_ARGS_CAR_AVAILABILITY` | `CarSelectScreen` (`FEmain.c`) | query: may the normally-locked extra vehicles (fire truck / buses / truck) be offered for this level? `result = 1` lifts the progression + single-player gate |
| `JER_EVENT_CAR_DATA_SOURCE` | `JER_ARGS_CAR_DATA_SOURCE` | `SetupResidentModels` (`mission.c`) | query: which city's `LEVELS\<city>` folder should this level read `CARMODEL_*` from, and which vehicles should be resident? `sourceLevel = 0..3` loads another city's vehicles (cross-city); `models[]` is the live `residentCarModels` array, so writing a model number into a slot (0..4 = traffic) puts that vehicle in the level. Runs before the model files are read. `modelSource[i]` (count entries) names the city whose LEVEL file supplies slot `i`'s geometry — -1 = the level's own — so one slot can take a foreign vehicle while the rest of the level is untouched. **It fires before the `wantedCar` pass**, so a handler may also set `wantedCar[]` to choose the car the player actually spawns in |
| `JER_EVENT_GET_WHEEL_BEND` | `JER_ARGS_QUERY_PTR` | wheel draw + physics | query: per-wheel bend array |
| `JER_EVENT_GET_WHEEL_DAMAGE` | `JER_ARGS_QUERY_INT` | surface roughness | query: cumulative wheel damage 0..4096 |
| `JER_EVENT_GET_WHEEL_PARAMS` | `JER_ARGS_WHEEL_PARAMS` | wheel yaw matrix + physics | query: deviation scales + scrub force |
| `JER_EVENT_GET_PHYSICS_PARAMS` | `JER_ARGS_PHYSICS_PARAMS` | `StepOneCar` (`wheelforces.c`) | query: gravity / angular settling / suspension rate |
| `JER_EVENT_GET_BUDDHA` | `JER_ARGS_QUERY_FLAG` | `bcollide.c` | query: damage-total clamp flag |
| `JER_EVENT_GET_IMPACT_INFO` | `JER_ARGS_IMPACT_INFO` | debug overlay | query: newest impact |
| `JER_EVENT_DRAW_WHEEL` | `JER_ARGS_DRAW_WHEEL` | `DrawCarWheels` | distort a wheel's vertex copy, or `hide=1` to skip it |
| `JER_EVENT_CAMERA` | `JER_ARGS_CAMERA` | `InitCamera` (chase cam) | adjust/override the camera transform |
| `JER_EVENT_GAME_START` | — | `State_GameInit` (`main.c:803`) | a level is starting (fresh / restart / next) — modules reset transient state; notification, no handler = nothing. Carries `JER_ARGS_GAME_START { seed }`: the debug `-seed` run seed, or 0, so a module can make itself reproducible under test |
| `JER_EVENT_CAMERA_LOOK` | `JER_ARGS_CAMERA_LOOK` | `TurnHead` | right-stick look input, before the stock look handling |
| `JER_EVENT_PED_INPUT` | `JER_ARGS_PED_INPUT` | ped control loop | rewrite the on-foot pad before it drives Tanner |
| `JER_EVENT_PED_MOVE` | `JER_ARGS_PED_MOVE` | `AnimatePed` (`pedest.c:638`) | player ped is about to move — the one point where a `pPed->speed` write survives the runner's per-frame re-arm; only a `TANNER_MODEL` ped with `padId >= 0`; no handler = stock speed |
| `JER_EVENT_PED_POSE` | `JER_ARGS_PED_POSE` | `DrawTanner` (`motion_c.c:1696`) | per-bone ROTATION window between `SetupTannerSkeleton` and `newRotateBones` (player ped only); module mutates `*Skel[i].pvRotation`; no handler = stock motion pose |
| `JER_EVENT_FRONTEND` | `JER_ARGS_FRONTEND` | `CutSceneCitySelectScreen` (`FEmain.c:3089`) | take-a-ride city confirm — module may rewrite `gameLevel`/`gameType`/`numPlayers`, or set `defer = 1` to run its own start over the frozen frontend; no handler = stock flow |
| `JER_EVENT_FRONTEND_MAIN_MENU` | `JER_ARGS_FRONTEND_ENTRY` | `MainScreen` (`FEmain.c`) | fired once per row while the title screen is built — rename (`label`), redirect to a module menu (`openMenu`), `disabled`, `hidden`; the engine always applies the struct; no handler = stock rows |
| `JER_EVENT_FRONTEND_ENTERED` | — | `State_InitFrontEnd` (`glaunch.c:325`) | the game (re)entered the frontend — modules drop per-run gameplay state (a forced car, live weapons, looped sounds) so nothing lingers or plays in the menus; no args, no handler = nothing |
| `JER_EVENT_FRONTEND_IDLE` | `JER_ARGS_FRONTEND_IDLE` | the frontend's idle timer (`FEmain.c`), before the attract demo starts | the timer is about to boot the attract demo after ~30 s with no input: a module sets `suppress = 1` to push it back. A multiplayer lobby is idle by definition, so with no veto the demo launches a level nobody asked for and blocks the main thread for the whole load, dropping every player who was joining |
| `JER_EVENT_PED_SKELETON` | `JER_ARGS_PED_SKELETON` | `newShowTanner` | pose Tanner's skeleton (phase 0) + draw extras (phase 1) |
| `JER_EVENT_MAP` | `JER_ARGS_MAP` | INPUT: `ControlMenu` (`pause.c:1505`) + `DrawFullscreenMap` (`overmap.c:1579`); DRAWN: `DrawFullscreenMap` (`overmap.c:1868`) | in-game map (`gShowMap`): a module returns `JER_RESULT_STOP` on INPUT to claim the pad (stock scroll/toggle skipped) and draws its own cursor on DRAWN; no handler = stock map |
| `JER_EVENT_DRAW_OVERLAY` | — | `DrawDebugOverlays` / HUD path | 2D overlay / HUD drawing |
| `JER_EVENT_PAUSE_MENU` | `JER_ARGS_PAUSE_MENU` | pause menu shell | module-owned menu state (labels + actions) |
| `JER_EVENT_SHUTDOWN` | — | `redriver2_main`, after the state loop | the game is exiting — release resources (sockets, files) |
| `JER_EVENT_CAR_PAD` | `JER_ARGS_CAR_PAD` | inside `ProcessCarPad` (`handling.c`), before the pedal assignment | take over the car's pedal semantics: set `handled` + write `cp->thrust`/`handbrake`/`wheelspin` — stock binds are skipped |
| `JER_EVENT_CAR_GEARBOX` | `JER_ARGS_CAR_GEARBOX` | `GetEngineRevs` (`gamesnd.c`) | retune the per-car gear/rev table (shift points + ratios + rev ceiling) |
| `JER_EVENT_CAR_REVS` | `JER_ARGS_CAR_REVS` | `ControlCarRevs` (`gamesnd.c`) | scale how fast the engine pitch slews to its target revs (rise/drop per frame) |
| `JER_EVENT_CAR_ENGINE_SOUND` | `JER_ARGS_CAR_ENGINE_SOUND` | `SoundTasks` (`gamesnd.c`) | scale/offset the player car's rev + idle channel pitch and volume |
| `JER_EVENT_CAR_ENGINE` | `JER_ARGS_CAR_ENGINE` | end of `ProcessCarPad` (`handling.c`) | transform engine force (thrust) + steering (wheel_angle) |
| `JER_EVENT_CAR_FRICTION` | `JER_ARGS_CAR_FRICTION` | end of `GetFrictionScalesDriver1` (`wheelforces.c`) | transform front/rear friction (grip) |
| `JER_EVENT_CAR_STEP` | `JER_ARGS_CAR_STEP` | top of `StepOneCar` (`wheelforces.c`) | observe car state (speed / velocity) |
| `JER_EVENT_CAR_TORQUE` | `JER_ARGS_CAR_TORQUE` | after `ConvertTorqueToAngularAcceleration` (`wheelforces.c`) | inject yaw torque (`aacc[1]`) |
| `JER_EVENT_CAR_DRAW` | `JER_ARGS_CAR_DRAW` | `DrawCar` (`cars.c`) | rotate the render-only body matrix (visual pitch/roll/yaw) |
| `JER_EVENT_CAR_DRAW_COLOR` | `JER_ARGS_CAR_DRAW_COLOR` | `DrawCarObject` (`cars.c`) | force a flat black body (totaled wreck) |
| `JER_EVENT_CAR_DAMAGE_FX` | `JER_ARGS_CAR_DAMAGE_FX` | `DrawCar` (`cars.c:1984`), after the stock smoke/fire test | the per-car damage smoke and fire: the car's health is passed in with the values the stock rule chose (type, both widths, flame), and `handled = 1` makes the module's values the ones emitted — no handler = exactly the stock emission |
| `JER_EVENT_PED_DRAW` | `JER_ARGS_PED_DRAW` | `newShowTanner` (`motion_c.c`) | ped body colour + per-instance palette: force a flat black (burning / bailed-out) or tinted ped, and/or select a recoloured outfit palette for this ped alone with `jer_ped_palette_select()` The flat colour reaches the polys as `plotContext.flatColour` under `PLOT_FLAT_COLOUR` (honoured in all three flat-shaded plot paths), **not** through `planeColours`: a pedestrian is drawn with `PLOT_NO_SHADE`, whose colour comes from `combo`, so holding `planeColours` alone - what the hook used to do - had no effect at all on a ped. |
| `JER_EVENT_GET_WALL_RESTITUTION` | `JER_ARGS_WALL_RESTITUTION` | `CarBuildingCollision` (`bcollide.c:1057`) | query: restitution scale 0..4096 (4096 = stock bounce) for a car hitting building/scenery; a low value cancels only the velocity into the wall (TMB-style absorb); no handler = stock (4096) |
| `JER_EVENT_LEVEL_LAUNCH` | `JER_ARGS_LEVEL_LAUNCH` | `State_GameStart` (`glaunch.c:309`) | pending level/gametype/player count/mission number are finalised but the level has not loaded — module rewrites them in place; no handler = the values the engine wrote are kept |
| `JER_EVENT_CMDLINE` | `JER_ARGS_CMDLINE` | `redriver2_main` (`main.c`), after the engine parsed its own argv | a module may pick up its own command-line shortcuts (e.g. mp's `-host`/`-join`); read-only `argc`/`argv`; no handler = ignored |
| `JER_EVENT_DRAW_WORLD` | — | `RenderGame2` (`main.c`), after `DrawAllTheCars` | draw world-space extras (projectiles, pickups) into the real OT — camera matrices are live; no handler = no-op |
| `JER_EVENT_GET_DAMAGE_SCALE` | `JER_ARGS_DAMAGE_SCALE` | `DamageCar` (`bcollide.c:604`) | query: scale 0..4096 (4096 = stock) on the damage a car takes from solid scenery; `impact` is the raw strike velocity (clamped to 2048000) so a module can ignore light scrapes; only used when a handler returns below 4096; no handler = stock (4096) |
| `JER_EVENT_CAR_VS_CAR` | `JER_ARGS_CAR_VS_CAR` | `DamageCar3D` (`bcollide.c:507`) | car-vs-car damage before `ApplyDamage` — `value` (the term actually applied) is in/out, `playerValue` is what a player car would have taken; no handler = stock `value` (clamped at 0) |
| `JER_EVENT_DRAW_MAP` | `JER_ARGS_DRAW_MAP` | `DrawMultiplayerMap` (`overmap.c:1043`), `DrawOverheadMap` (`overmap.c:1218`), `DrawFullscreenMap` (`overmap.c:1823`) | map draw, after the player blip — module plots extra markers with `DrawTargetBlip` using the same `flags`; `fullscreen` distinguishes the map; no handler = stock blips only ; a module may also set `suppressStockBlip` to draw every player itself, which is what suppresses the engine's own blip (it holds `NumPlayers` at 1, so that blip is the local player's) |
| `JER_EVENT_EXPLOSION_SPAWN` | `JER_ARGS_EXPLOSION_SPAWN` | `AddExplosion` (`job_fx.c`) | attach a parametric FX profile to a new explosion: size (`speed`/`hscale`/`rscale`), `tint*`, `yawRate`, `collide`, `colScale`, and rewrite `type` |
| `JER_EVENT_EXPLOSION_DRAW` | `JER_ARGS_EXPLOSION_DRAW` | `DrawExplosion` (`job_fx.c`) | tint/spin the stock bang, or set `override` to draw your own effect |
| `JER_EVENT_EXPLOSION_COLLIDE` | `JER_ARGS_EXPLOSION_COLLIDE` | `ExplosionCollisionCheck` (`bomberman.c`) | query: may this explosion push/damage this car, and at what box scale |
| `JER_EVENT_MP_FRONTEND` | `JER_ARGS_MP_FRONTEND` | `MainScreen` + `HandleKeyPress` (`FEmain.c`) | the main-menu Multiplayer entry / the multiplayer gamemode screen is being entered: a module may claim it (`claimed = 1`) and run its own menu; also enables the entry without two pads |
| `JER_EVENT_NET_INPUT` | `JER_ARGS_NET_INPUT` | car-control loop (`main.c`) | a player car's pad is about to drive it: a module may substitute a remote player's input (`handled = 1`) |
| `JER_EVENT_NET_CAR_STATE` | `JER_ARGS_NET_CAR_STATE` | `StepOneCar` (`wheelforces.c`) | per player car: capture (`apply = 0`) or apply (`apply = 1`) a synced transform (host state-resync) |
| `JER_EVENT_NET_PLAYERS` | `JER_ARGS_NET_PLAYERS` | top of `StepSim` (`main.c`) | notification: the local player pad ids, so a module maps them to peers |
| `JER_EVENT_NET_RECV` | `JER_ARGS_NET_RECV` | the mp addon bridge (`jer_net.h`) | an inbound channel payload for a module that registered it |
| `JER_EVENT_NET_SPAWN` | `JER_ARGS_NET_SPAWN` | `InitGameVariables` (`main.c`) | a level's player cars are about to be created: a network module adds the remote players (fills `PlayerStartInfo[slot]`, raises `numPlayersToCreate`); extra slots get negative pad ids |
| `>= JER_EVENT_MODULE_CUSTOM` | module-defined | modules | custom events |

## The explosion FX events (Caine's Crossfire weapons use these)

`AddExplosion(pos, type)` is the single spawn point for every explosion in
the game (mission bangs, thrown bombs, weapon impacts). Three hooks turn one
into a *parametric* effect without touching the engine's draw code:

- **`JER_EVENT_EXPLOSION_SPAWN`** fires once, after the slot is armed and the
  stock size for `type` seeded. The args carry the live values; a module may
  rewrite any of them (and `type` — usually to a stock bang so the engine's
  sound/collision branches stay valid). A custom `type` id (>= 1000) has no
  engine size defaults, so a module that uses one MUST set
  `speed`/`hscale`/`rscale` itself. `tint* = -1` keeps the stock colour,
  `collide = 0` makes it visual-only, `colScale` scales the collision box
  (4096 = stock), `yawRate` is extra spin in PSX angle units per frame, and
  `fxId` is a free module tag carried on the explosion.
- **`JER_EVENT_EXPLOSION_DRAW`** fires per explosion per frame while it is
  drawn (camera matrices live). It writes `tint*`/`yaw` in place, or sets
  `override = 1` after drawing its own effect (the stock hemisphere is then
  skipped for this explosion).
- **`JER_EVENT_EXPLOSION_COLLIDE`** is a query: `result` (defaulted from the
  explosion's own `collide` flag) decides whether the stock push/damage runs,
  and `colScale` tightens the collision box.

The pool is `MAX_EXPLOSION_OBJECTS` (16; raised from 5 for barrage weapons);
`AddExplosion` recycles the oldest slot if it ever fills.

## Query events

Query events use the args struct as a result slot. The engine initializes
the fields to their "no module" defaults (NULL / 0), fires the event, and
reads the (possibly updated) fields back. No handler = stock behavior.

## The camera events (d2pl uses these)

- **`JER_EVENT_CAMERA`** fires at the end of the chase-cam update once the
  engine has computed `camera_position` / `camera_angle`. A module may move
  the position, re-aim the angle, and set `args->override = 1` — the engine
  then rebuilds its view matrices from the module's values so nothing else
  fights the transform. The args also carry `basePos` (the chased point),
  `baseDir` (the chased facing), `carSpeed` and `inCar` so a module can run
  its own full chase-cam math instead of poking at the stock result.
- **`JER_EVENT_CAMERA_LOOK`** fires at the top of `TurnHead` with the
  player, the camera pad bits, and the raw right-stick analog
  (`stickX`/`stickY`). A module drives the look (e.g. GTA-style orbit by
  writing `lp->cameraAngle` + `gripOrbit = 1`, which makes the engine skip
  its settle-back lerp) and may set `suppress` to skip the stock L2/R2
  look-left/right/back handling.
- **`JER_EVENT_PED_INPUT`** fires right before `ProcessTannerPad` with the
  pad that is about to drive the on-foot player. The module can rewrite the
  pad bits (or write `lp->dir` directly, as d2pl does for camera-relative
  movement). No handler = stock pad handling.
- **`JER_EVENT_PED_SKELETON`** fires twice per player-ped draw from
  `newShowTanner`: phase 0 before the bone accumulation (override
  `Skel[i].vCurrPos` to pose the arm), phase 1 after (read `vJPos` for
  world-space joint positions and draw extra meshes). `shadow` is set when
  the pass is for the shadow, so modules can skip there.
- **`JER_EVENT_PED_MOVE`** fires inside `AnimatePed` right before the
  player ped's position is advanced (`pedest.c:638`), for a `TANNER_MODEL`
  ped with `padId >= 0`. `ProcessTannerPad` and the runner re-arm `speed =
  MAXRUNSPEED` every frame, so this is the one point where a `pPed->speed`
  write survives the advance — a module scales the run speed here (e.g. from
  the analog deflection magnitude, lerped for stand-start momentum). No
  handler = stock speed.
- **`JER_EVENT_PED_POSE`** fires in `DrawTanner` between
  `SetupTannerSkeleton` and `newRotateBones` (`motion_c.c:1696`), player ped
  only. `args->skel` is the engine's `BONE* Skel[]`; each
  `Skel[i].pvRotation` points into the raw motion-frame bytes that
  `newRotateBones` reads immediately after the hook, so a rotation write here
  is that frame's last word on the bone (the `PED_SKELETON` phase-0 hook
  remains the position channel). Use the `jer_anim.h` helpers to resolve
  bones.

## The car handling events (collisiondevil uses these)

Five transform/observe hooks added for handling overhauls. Each fires once
per car per physics frame (or per draw, for `CAR_DRAW`) and is a no-op with
no handler.

- **`JER_EVENT_CAR_REVS`** fires at the top of `ControlCarRevs` once per
  active car per frame. `args->revRise` / `args->revDrop` default to the stock
  slew limits (`maxrevrise` 1600, `maxrevdrop` 1440 — the maximum the pitch
  may climb / fall toward its target revs each frame). Scaling them up makes
  the engine rev up faster (and drop faster on shifts/let-off); no handler
  leaves the stock behavior untouched.

- **`JER_EVENT_CAR_PAD`** fires inside `ProcessCarPad` right before the
  stock face-button assignment (`handbrake`/`wheelspin`, then the
  brake/accelerate `thrust` block), after leave-car/horn and the locked-car
  clamp. A module that wants to rebind the car's buttons does it here:
  `args->pad` carries the engine-native CAR_PAD_* bits for the car (for a
  player these are exactly the physical→PS-bit mapping from `config.ini`).
  Set `args->handled = 1` and write `cp->thrust` / `cp->handbrake` /
  `cp->wheelspin` yourself — the stock pedal assignment is then skipped, so
  a physical button never double-fires its original action. `args->live` is
  1 only for genuine live player input (not AI/lead/cutscene/replay pads,
  and not the clamped locked-car brake/handbrake state), which is the only
  case an override should normally act on. Steering (`wheel_angle`) is
  assigned after the hook and is unaffected by `handled`.

- **`JER_EVENT_CAR_ENGINE`** fires at the end of `ProcessCarPad` after
  `cp->thrust` and `cp->wheel_angle` are computed. A module scales
  `args->thrust` (engine force — derived from `car_cosmetics[].powerRatio`)
  and `args->wheel_angle` (steering) in place; the engine writes them back
  to `cp->thrust` / `cp->wheel_angle`.
- **`JER_EVENT_CAR_FRICTION`** fires at the end of
  `GetFrictionScalesDriver1` once the front/rear friction scales are
  finalized. A module scales `args->frontFS` / `args->rearFS` in place
  (e.g. drop rear grip to induce oversteer). Values are derived from
  `car_cosmetics[].traction` × `handlingType[].frictionScaleRatio`.
- **`JER_EVENT_CAR_STEP`** fires at the top of `StepOneCar`; read-only —
  `args->speed`, `velX`, `velZ`, `avelY` let a module detect drift and
  capture G-force without writing anything.
- **`JER_EVENT_CAR_TORQUE`** fires after
  `ConvertTorqueToAngularAcceleration`; a module sets `args->yawTorque`,
  which is added to `cp->hd.aacc[1]` (yaw angular acceleration) for a
  predictable powerslide kick.
- **`JER_EVENT_CAR_DRAW`** fires in `DrawCar` over a render-only copy of
  `cp->hd.drawCarMat`. A module rotates `args->matrix` (a `MATRIX*`) for
  visual pitch/roll/yaw; the physics matrix (`cp->hd.where`) and collision
  box are never touched.

- **`JER_EVENT_CAR_DAMAGE_FX`** fires in `DrawCar` (`cars.c:1984`), at the point
  where the engine decides a car's damage smoke and its engine fire — once per
  drawn car per frame, full-detail cars only (the same branch as the shadow and
  the exhaust). The engine fires it with what its own rule produced:
  `smokeType` (a `SMOKE_*` type, 0 = none) and both widths, `flame` and both fire
  widths, and `health` (0..100, the car's `totalDamage` against its own cap,
  per-pad for a player car — so a module can compare it to a percentage
  directly). Set `handled = 1` to have the module's values emitted instead. The
  stock rule is per-zone `ap.damage` (>2000 white, >3000 black) with the fire
  only on a totaled car that has almost stopped, which is why a module wants
  this: a ladder by health (smoke from half, fire from a quarter) cannot be
  expressed by that test.
  The engine's speed gates still apply with `handled = 1` — the smoke only under
  ~98 speed units, the fire only under ~7 and never in reverse — because they
  are the shared smoke pool's budget (`MAX_SMOKE`), not part of the ladder.
  The emitters behind it (`AddSmokingEngineTyped`, `AddFlamingEngineSized`,
  `cosmetic.c`) take the type and the sizes as arguments for exactly this; the
  stock `AddSmokingEngine`/`AddFlamingEngine` are those with the original
  numbers. No handler = the stock emission, unchanged.

## The damage events (Caine's Crossfire uses these)

Three hooks around the collision damage path, all in `bcollide.c`; each is a
no-op with no handler.

- **`JER_EVENT_GET_DAMAGE_SCALE`** — query fired in `DamageCar`
  (`bcollide.c:604`) just before the scenery (building/wall) hit is applied to
  a car. `result` defaults to 4096 (= stock) and is only used when a handler
  lowers it (`value = value * result >> 12`), so a module softens scenery
  damage. `impact` carries the raw strike velocity (the same term the car-vs-car
  event calls `strikeVel`, clamped to 2048000; the engine only reaches here at
  `strikeVel >= 20480 && hd.speed > 9`), so a module can drop light scrapes
  entirely by returning 0 below its own threshold. No handler = full stock
  damage.
- **`JER_EVENT_GET_WALL_RESTITUTION`** — query fired in `CarBuildingCollision`
  (`bcollide.c:1057`) once a building/scenery hit is detected and the stock
  reaction impulse computed. `result` defaults to 4096 (= stock bounce); when
  a module lowers it the engine cancels only the velocity into the wall (the
  normal component), keeps the tangential component so the car scrapes along,
  and reflects just `result` of the normal — a hard-stop wall (the TM2/TMB
  "collision forgiveness"). 0 = pure absorb, 4096 = full bounce. No handler =
  the stock outward impulse + wall spin.
- **`JER_EVENT_CAR_VS_CAR`** — fired in `DamageCar3D` (`bcollide.c:507`) when
  two cars collide, right before `ApplyDamage`. The engine computes `value`
  (the term actually applied to this car — the non-player branch uses a
  harsher multiplier than the player one) and `playerValue` (what a
  player-controlled car would take for the same impact). A module overwrites
  `value` — e.g. give an owned opponent the player damage model, or scale the
  exchange — and the engine clamps it at 0. No handler = stock `value`.

## The map events (sandbox / Caine's Crossfire use these)

- **`JER_EVENT_MAP`** (`JER_ARGS_MAP`) fires while the in-game map
  (`gShowMap`) is up, in two actions. `JER_MAP_ACTION_INPUT` fires from the
  pause shell's `ControlMenu` (`pause.c:1505`) and from `DrawFullscreenMap`
  (`overmap.c:1579`) with the current pad bits; a module returns
  `JER_RESULT_STOP` to claim the pad, and the engine then skips its own map
  scrolling / toggle handling. `JER_MAP_ACTION_DRAWN` fires at the end of
  `DrawFullscreenMap` (`overmap.c:1868`), after the map tiles, so a module
  draws its own cursor on top. The `value` in the INPUT action is
  caller-dependent (`Pads[0].direct` in overmap.c, the pause pad in pause.c),
  so handlers should read `Pads[0]` directly instead of relying on it. No
  handler = stock map controls and drawing.
- **`JER_EVENT_DRAW_MAP`** (`JER_ARGS_DRAW_MAP`) fires right after the
  player's own blip on each map the engine draws: `DrawMultiplayerMap`
  (`overmap.c:1043`, `flags = 0x20 | 0x2`), `DrawOverheadMap`
  (`overmap.c:1218`, `flags = 3`) and `DrawFullscreenMap` (`overmap.c:1823`,
  `flags = 14`). A module plots extra markers with
  `DrawTargetBlip(pos, r, g, b, flags)` using the same `flags` value so they
  land in the right place; `fullscreen` distinguishes the fullscreen map from
  the overhead one. No handler = only the stock blips are drawn.

## The level-start events (levelhacks / Caine's Crossfire / d2pl use these)

Three notification hooks fire around launching a level; each is a no-op with
no handler.

- **`JER_EVENT_GAME_START`** fires in `State_GameInit` (`main.c:803`) once a
  level is starting — a fresh launch, a restart, or the next mission — after
  controllers are re-opened, before the state switches to `STATE_GAMELOOP`. It
  carries no args (fired with `NULL`); modules use it to reset transient state
  (e.g. the sandbox closes its menu so it does not reopen unprompted, and
  Caine's Crossfire re-arms its per-level inventory).
- **`JER_EVENT_LEVEL_LAUNCH`** fires at the end of `State_GameStart`
  (`glaunch.c:309`), after the pending level / gametype / player count /
  mission number are finalised but before the level loads. All four fields are
  in/out and the engine writes them back, so a module may redirect the launch
  (e.g. bump a take-a-ride mission number into the multiplayer-map range). No
  handler = stock values.
- **`JER_EVENT_FRONTEND`** fires in `CutSceneCitySelectScreen`
  (`FEmain.c:3089`) when the player confirms a take-a-ride city (CROSS),
  before the frontend leaves. `gameLevel` / `gameType` / `numPlayers` are
  in/out; setting `defer = 1` makes the module own the start (its own menu
  runs over the frozen frontend) and the engine returns without scheduling the
  level. No handler = stock flow.
- **`JER_EVENT_FRONTEND_MAIN_MENU`** fires once for every row of the title
  screen while `MainScreen` builds it (before the stock Multiplayer routing).
  `JER_ARGS_FRONTEND_ENTRY { index, label[32], hidden, disabled, openMenu }` is
  in/out and the engine applies it (`openMenu >= 0` opens that registered module
  menu), so a module can rename a row, point it at its own screen, or omit it
  and put something else in its place.
- **`JER_EVENT_FRONTEND_ENTERED`** fires at the end of `State_InitFrontEnd`
  (`glaunch.c:325`) — the game has (re)entered the menus, which is also the way
  back from a match. Modules drop their per-run gameplay state here (a forced
  car, live weapons, looped sounds) so nothing lingers or plays in the frontend.
  No args, notification only.

## The multiplayer events (the mp module uses these)

Five hooks added for LAN multiplayer (`JERICHO/MODS/mp`):

- **`JER_EVENT_MP_FRONTEND`** — fired from the frontend main screen
  (`MainScreen`, as a query, to enable the multiplayer entry without a second
  pad) and from `HandleKeyPress` when the main-menu Multiplayer button
  (`JER_MP_FE_ENTER_MENU`) or the multiplayer gamemode screen
  (`JER_MP_FE_GAMEMODE`) is confirmed. A module that sets `claimed = 1` owns
  the press: the engine skips the stock navigation, so the module can draw its
  own host/join menu over the frozen frontend. No handler = stock flow.
- **`JER_EVENT_NET_INPUT`** — fired in the car-control loop (`main.c`) right
  after a `CONTROL_TYPE_PLAYER` car's pad is read and before `ProcessCarPad`.
  A module writes `pad` (engine-native bits) and sets `handled = 1` to drive
  the car from remote input instead. No handler = the stock pad.
- **`JER_EVENT_NET_CAR_STATE`** — fired per player car in `StepOneCar`
  (`wheelforces.c`). With `apply = 0` the module reads the transform (`x/y/z`
  from `hd.where.t[]`, `heading` from `hd.direction`) for capture; with
  `apply = 1` the engine writes the module's values back (a client snapping to
  the host). No handler = no-op.
- **`JER_EVENT_NET_PLAYERS`** — fired once per frame at the top of `StepSim`
  with the local player pad ids (`padIds[]`, `count`), so a module can map them
  to network peers. No handler = no-op.
- **`JER_EVENT_NET_RECV`** — the JERICHO addon network bridge (see
  `jer_net.h`): the mp module delivers every inbound channel payload
  (`channel`, `peer`, `data`, `len`) to modules that registered the channel.

`JER_EVENT_LEVEL_LAUNCH` also carries in/out `timeOfDay`/`weather`, so a host
can impose its chosen time of day and weather on the level load (`LoadMission`
folds them in when >= 0).

## The pause menu bridge

`JER_EVENT_PAUSE_MENU` is the pause shell ↔ module contract. The engine's
`pause.c` owns the menu items; modules own the state. Actions are
multiplexed: the crumple module has one action per toggle; the d2pl module
uses two multiplexed actions — `JER_PAUSE_D2PL_GET_LABEL` (with the item id
in `args->value`, the label returned via `args->result`) and
`JER_PAUSE_D2PL_ADJUST` (item id in `value`, direction -1/0/+1 in `result`).

`JER_PAUSE_OPEN` is special: it fires when the player presses START
(single-player) *before* the engine pause opens. A module that replaces the
pause menu (the sandbox overlay) claims the press with `JER_RESULT_STOP` and
the engine pause never opens.

## Diagnostics

At boot (and on every Mods-menu reload) the runtime logs into
**`REDRIVER2.log`** (via the PsyX logger, wired in `main.c`):
- one line per compiled-in module (id, version, enabled, state, author,
  deps) — `state=INVALID` means SDK/dependency validation failed and the
  reason is logged right above it;
- one line per registered hook handler (event, owning module, priority).

So a quick scan of `REDRIVER2.log` right after boot tells you which of the
hooks in this table are actually live, and which modules are running them.

## Override slots

`MOD_CONTEXT::jer_override` swaps engine function-pointer slots. Currently
one slot is consumed by the engine: `JER_OVERRIDE_SLOT_SIM` (the world step,
`StepSim`) — the sandbox module uses it for time-scale. The remaining slots
are reserved for future whole-behavior replacement.

## Persistent config

Modules get a persistent key/value store via the JERICHO config API
(`jer_config.h`): each module owns a text file in
**`JERICHO/CONFIG/<modid>.ini`** (the folder is created at boot). Files are
read lazily on first access and rewritten on every `set`, so values survive
restarts with no flush step. On PSX the API is a no-op returning the
provided defaults. d2pl persists its camera/weapon settings there; the
sandbox persists its "replace pause menu" toggle.
