// COMBAT D2 — Twisted Metal: Black style arcade handling for REDRIVER2.
//
// A compiled-in JERICHO deep mod. Unlike COLLISIONDEVIL (which scales the
// stock wheel/suspension sim), Combat D2 replaces the *horizontal* motion
// with a point-mass rigid body:
//
//   * velocity is controlled directly from throttle (no engine/gear/suspension)
//   * yaw is controlled directly from steering (target yaw rate, works at zero
//     speed — rotate in place); steering authority never drops during a slide
//   * the Tight Turn (Triangle / handbrake) is a separate acute-pivot
//     authority — an intentional "cheat" against realism for combat positioning
//   * brakes are fast and proportional (strong scrub at speed, smooth taper)
//   * lateral grip is high and only slips a little, so skids are brief and
//     never a loss-of-control spiral
//   * walls absorb momentum (hard stop) instead of bouncing
//
// Feel is engineered, not simulated: responsive and forgiving (TMB's north
// star), with weight/control differentiated per vehicle from the chassis
// stats so a light car is instant and a truck is slow but heavy.
//
// Vertical motion (gravity + ground lift, cp->hd.acc[1]) and roll/pitch
// (cp->hd.aacc[0]/[2]) are left to the stock code so the car still rides the
// terrain. Collision impulses are stock (mass-based "push"); our grip + drag
// make the recovery forgiving, which is the TM2 feel.

#ifndef CAINESCROSSFIRE_H
#define CAINESCROSSFIRE_H


// ==============================================================
// UNITS & ENGINE-WRITE CONSTANTS
// ==============================================================

// --------------------------- units --------------------------------------
//
// Two unit systems meet here:
//   * speed  : "game speed units" == world-units per frame == cp->hd.speed.
//              linearVelocity is that × 4096 (fixed point, ONE = 4096).
//   * yaw    : PSX angle units, 4096 == 360° (the game's native angle).
//
// The TM2 reference gives SI values (topSpeed 20 m/s, handling 120°/s, …).
// Those are quoted in the comments below; the active numbers are the game-unit
// equivalents, folded to per-frame (the sim is frame-locked) so they can be
// tuned by play exactly like the stock physics.

// angularVelocity[1] per unit of yaw rate (PSX-units/frame). Derived from the
// quaternion integrator in GlobalTimeStep (handling.c): AV = avel >> 13 is the
// half-angle in fixed point, so avel = yawRate × π × 8192.
#define CD2_AV_PER_UNIT     25736

// wheel_angle magnitude treated as full-lock steer (stock regular max = 352).
#define CD2_STEER_MAX       352


// ==============================================================
// INPUT: TMB LAYOUT + TIGHT TURN TRIGGER (who presses what)
// ==============================================================

// Tight Turn (TMB): an acute forced pivot DERIVED from the car's normal turn
// authority — pivot rate = normal handling x CD2_TIGHT_MULT, so it scales
// with the car's own steering stat/control instead of a fixed extra spin.
// Pivot direction comes from the (latched) steer input; the pivot bleeds a
// little forward speed so holding gas produces a short drift-slide instead
// of a dead stop, and at low speed it spins nearly in place. The trigger is
// selectable so it stays rebindable:
//   CD2_TIGHT_INPUT_HANDBRAKE = Triangle (engine's handbrake),
//   CD2_TIGHT_INPUT_WHEELSPIN = Circle  (engine's wheelspin/burnout bit),
//   CD2_TIGHT_INPUT_OFF       = disabled.
// (Physical buttons themselves are remapped by the engine's config.ini.)
#define CD2_TIGHT_MULT          6230   // fp: pivot = normal handling x this (something like 1.5x but idk)
#define CD2_TIGHT_ANG_MULT      1.6    // yaw angular step multiplier during a pivot
#define CD2_TIGHT_BLEED         96     // fp/frame: horizontal speed lost while pivoting (~2.3%)
#define CD2_TIGHT_STEER_MIN     16     // |wheel_angle| that (re)latches a pivot direction
#define CD2_TIGHT_ROT_STOP_SPEED 100  // speed units/frame: below this, a released
                                      // tight turn's residual spin is snapped off

enum
{
	CD2_TIGHT_INPUT_HANDBRAKE = 0,
	CD2_TIGHT_INPUT_WHEELSPIN = 1,
	CD2_TIGHT_INPUT_OFF = 2
};

#define CD2_TIGHT_ENABLED_DEFAULT   1
#define CD2_TIGHT_STRENGTH_DEFAULT  100   // 0..100 pivot authority
#define CD2_TIGHT_INPUT_DEFAULT     CD2_TIGHT_INPUT_HANDBRAKE

// TMB Classic in-car face-button layout (default ON). It is implemented as
// a JER_EVENT_CAR_PAD override, NOT a rebinding of physical buttons: the
// module writes the car's pedal state itself and the engine skips its stock
// face-button assignment, so the original binds never double-fire.
//   Square-position = Gas, Circle = Brake, and the other primary face button
//   (Cross) = Tight Turn. Triangle is left unbound for the car (in TMB it is
//   rear-view / nothing combat relevant here); get-in/get-out stays on the
//   dedicated L3 exit. On foot, ped controls are untouched.
#define CD2_TMB_BUTTONS_DEFAULT     1
// Some pads label the LEFT face button "X" (Xbox X / PS Square). tmb_tight
// picks which PHYSICAL button carries the Tight Turn when the TMB layout is
// on: 0 = Cross/bottom (PS "X", Xbox A), 1 = Square/left (Xbox X). Gas is on
// the other one.
#define CD2_TMB_TIGHT_DEFAULT       0
// thrust magnitude the override writes for gas (- for brake); only the SIGN
// matters to the cainescrossfire torque model, but the magnitude feeds the stock
// wheel/pitch pass, so it should sit near the stock accel force (~power*4915).
#define CD2_TMB_THRUST              4215


// ================ GRIP, SLIDE & RECOVERY ========================

// Grip default (fixed point /frame; 1800/4096 ≈ 0.44/frame ≈ 13 s⁻¹ — TMB keeps
// skids short and sparse, so base grip is high and only drops a little):
#define CD2_GRIP            1100
#define CD2_SLIP_REDUCTION  3     // /10 → max 60% grip drop at full slip
                                    // (never past 6: a drop over 100% makes
                                    // grip negative = instant blow-up)

// Iconic TMB slide: while the Tight Turn is held AND the car is fast enough,
// traction is only PARTIALLY released — the car keeps real friction
// (CD2_SLIDE_GRIP_FRAC of full grip), so the skid itself scrubs speed and
// arcs under the pivot like a stock friction skid. The heavy grip overrides
// are walked back: cainescrossfire nudges the grip multiplier; it does not switch
// the skids off.
#define CD2_SLIDE_MIN_SPEED  100     // speed units/frame (below: low-speed spin)
#define CD2_SLIDE_GRIP_FRAC  900     // fp: grip multiplier during the slide
                                    // (900/4096 ≈ 22% of normal grip)
// Big-skid ice lock: once the slide is sliding SIDEWAYS harder than
// CD2_SKID_LOCK_LAT, the car stops arcing toward the nose entirely and keeps
// its ORIGINAL momentum line — grip drops to CD2_SKID_LOCK_GRIP so the
// velocity is never scrubbed toward the rotated heading (pure TMB ice drift).
#define CD2_SKID_LOCK_LAT    40      // speed units: lateral velocity threshold
#define CD2_SKID_LOCK_GRIP   96      // fp: grip multiplier once locked (~2.3%)


#define CD2_SLIDE_BLEED         32     // fp/frame: bleed while tight-sliding (~0.8%).
                                        // Tight Turn is NOT a brake: on ice the car
                                        // keeps its momentum and sheds speed slowly
                                        // (~half speed every 1.5s), forgiving to steer.
#define CD2_SLIDE_ACCEL_FRAC    1024   // fp: accel fraction allowed during a tight slide (0.25x)
// Velocity recovery after a slide ends: the old "hookup" scrubbed the lateral
// component at ~93%/frame, which killed the car's speed whenever the pivot had
// rotated the heading away from the travel direction (it stopped dead). TMB
// releases instead CARRY the momentum: for CD2_RECOVER_FRAMES after letting
// off the Tight Turn, the velocity is rotated back onto the heading while its
// magnitude is conserved, then grip returns GRADUALLY over CD2_GRIP_RAMP_FRAMES
// (never a snap back to full grip).
#define CD2_RECOVER_FRAMES    8     // frames of magnitude-preserving recovery
#define CD2_RECOVER_RATE      256   // fp: fraction of the heading gap closed per
                                    // recovery frame (256/4096 = 6.25%)
#define CD2_GRIP_RAMP_FRAMES  10    // frames to ramp grip back up after recovery

// --------------------------- default stats -------------------------------
//
// Speed defaults (world-units/frame):
#define CD2_TOP_SPEED       440   // speed-units/frame (raw slider value; the
                                    // physics top is CD2_SPEED_SCALE x this)
#define CD2_SPEED_SCALE     2048  // fp: effective top speed multiplier (0.5x =
                                    // ~130 at the default slider value)
#define CD2_REVERSE_FRAC    4096  // fp: reverse cap = forward top x this
                                    // (4096 = same as forward — TM drives
                                    // backwards as fast as it does forwards)
#define CD2_ACCEL           16     // speed-units/frame² (0→top in ~1s)
#define CD2_BRAKE           14    // PEAK brake decel, speed-units/frame², applied
                                  // proportionally (strong at speed, taper near 0)
#define CD2_BRAKE_FLOOR     1024  // fp: fraction of peak brake kept at standstill
                                  // (1024/4096 = 25%) so stopping is never asymptotic
#define CD2_REVERSE_ACCEL_FRAC 2048 // fp: reverse accel = brake x this (2048/4096 = 0.5)
#define CD2_DRAG            34    // fixed point /frame: 48/4096 ≈ 1.2%/frame (coast)

// Yaw defaults (PSX-units/frame; 4096 = 360°):
#define CD2_HANDLING        35    // max yaw rate  (≈ 120°/s at 30 fps)
#define CD2_ANGULAR_ACCEL   18    // yaw accel toward target (≈ 360°/s²)
#define CD2_YAW_SPEED_FALLOFF 800   // fp: TM2 speed-sensitive yaw falloff. 0 = off;
                                  // ~1200-1600 turns down yaw authority as speed
                                  // rises so the car can't spin out at top speed.
#define CD2_YAW_DECAY       2048  // fp: centering step multiplier while the yaw
                                  // returns to center (2048 = 2x) — the car stops
                                  // spinning promptly instead of carrying rotation




// ==============================================================
// SUSPENSION, GRAVITY & ANGULAR SETTLING  (JER_EVENT_GET_PHYSICS_PARAMS)
// ==============================================================
// These feed the engine's StepOneCar via JER_EVENT_GET_PHYSICS_PARAMS. Stock
// constants are: gravity -7456, angular damping 128, spring rate 230, spring
// damping 100 (wheelforces.c). Higher angularDamping settles pitch/roll/yaw
// faster (less floaty). Higher springRate/springDamping = tighter, less
// bouncy suspension. Values here are compile-time tunables.
#define CD2_GRAVITY           -10922   // vertical accel (D1 used -10922)
#define CD2_ANGULAR_DAMPING   512     // stock 128; 256 = 2x faster angular settle
#define CD2_SPRING_RATE       520     // stock 230 (stiffer)
#define CD2_SPRING_DAMPING    320     // stock 100 (less bounce)

// ==============================================================
// VISUALS: BODY ROLL, CAMERA & FOV
// ==============================================================

// Visual: lateral velocity (speed units) → body roll (PSX angle units).
#define CD2_ROLL_GAIN       15     // roll = -latVel * gain, clamped below
#define CD2_BODY_MAX_ROLL   14    // ~2° lean (TMB: weight felt, not exaggerated)
#define CD2_ROLL_LERP       2     // exponential settle divisor

// Camera FOV pull (same trick as COLLISIONDEVIL): scr_z reduction at speed.
#define CD2_FOV_PULL_SCRZ   60
#define CD2_FOV_REF_SPEED   120

// TMB-style chase framing (applied every frame to the player's main chase
// cam, cameraView 0): after the engine places the camera it is nudged CLOSER
// to the car on the ground plane and eased LOWER toward the car's base. Both
// are relative fractions of the gap (fixed point 4096) so they can never
// overshoot into the car; 0 disables that axis. 4096 = keep the stock frame.
#define CD2_CAM_PULL        350   // fp: fraction of the gap to the car closed
#define CD2_CAM_LOW         0      // fp: fraction of the height gap closed (0 = keep stock height)


// ==============================================================
// COLLISION: WALLS ABSORB MOMENTUM
// ==============================================================

// Wall collision: cainescrossfire returns this as the wall restitution, but the
// engine now applies it to the INTO-wall (normal) component only: 0 = walls
// fully absorb the impact and the car keeps scraping tangentially along the
// wall (TM2 "collision forgiveness"), 4096 = stock outward bounce + spin.
#define CD2_WALL_KEEP       384

// ==============================================================
// WEAPONS
// ==============================================================
//
// Car weapons live in the weapon framework under weapons/. weapon.h owns
// the public types: CD2_WEAPON_DEF (the consistent per-weapon field set)
// grouped by functional class (raycast / projectile / aoe / drop). Each
// weapon is one def row + a fire function in its own class folder; the
// core pool code drives them. See weapons/core/weapon.h.

// ----------------------- engine audio / gearbox ---------------------------
//
// cainescrossfire drives speed directly, so the stock rev model (gamesnd geard)
// keeps climbing with cp->hd.wheel_speed and would wind far past a sane
// redline. While a player drives we retune the gear table via
// JER_EVENT_CAR_GEARBOX: short, snappy lower gears, and a TALL top gear whose
// ratio levels the pitch at the car's cainescrossfire top speed — revs then cap at
// CD2_REV_CEILING no matter how fast the point-mass model goes.
//   ws (rev-model speed) = car speed x CD2_WS_PER_SPEED/4096
//   (wheel_speed is ~4096x speed and GetEngineRevs shifts it >> 11 => 2x)
// Gear boundaries are fractions of the car's per-vehicle top speed:
//   gear0..1 | gear1..2 | gear2..3 top out at those fractions of top speed
//   (the LAST gear tops out at 1.0 = top speed), and each lower gear hits
//   CD2_GEAR_SHIFT_REVS just before the upshift so shifts sound punchy.
#define CD2_GEAR_AUTO         1     // 0/1: retune player cars' gear tables
#define CD2_WS_PER_SPEED      8192  // fp: ws per 1.0 speed unit (8192/4096 = 2x)
#define CD2_GEAR_1_FRAC       1000   // fp: gear0 tops here (≈ 12% of top speed)
#define CD2_GEAR_2_FRAC       2100  // fp: gear1 tops here (≈ 27%)
#define CD2_GEAR_3_FRAC       3250  // fp: gear2 tops here (≈ 43%)
#define CD2_GEAR_SHIFT_REVS   23000 * 0.7 // revs at the top of gears 0..2 (pitch peak;
                                    // matches stock redline scale)
#define CD2_REV_CEILING       23000 * 0.8 // top-gear revs AT top speed; hard rev clamp
#define CD2_GEAR_DOWN_FRAC    3686  // fp: downshift point = prev gear top x this
                                    // (3686/4096 ≈ 0.9; hysteresis vs the upshift)
// Engine pitch is hd.revs, slewed every frame toward a target set by the
// gearbox above (CD2_GEAR_SHIFT_REVS / CD2_REV_CEILING choose WHERE the
// pitch sits). Two independent knobs shape the feel:
//   * CD2_REV_RISE_SCALE / CD2_REV_DROP_SCALE change HOW FAST the pitch gets
//     there. They multiply the engine's per-frame slew caps (stock
//     maxrevrise = 1600, maxrevdrop = 1440 in gamesnd.c, exposed to modules
//     by JER_EVENT_CAR_REVS). 4096 = stock lag. Raise to make the engine
//     snap to redline and fall hard on shifts / let-off (≈ 2-4x =
//     8192-16384); below 4096 is lazier.
//   * The CD2_SND_* knobs adjust the two engine channels (rev + idle) at the
//     mixer (JER_EVENT_CAR_ENGINE_SOUND). Pitch is SPU pitch (4096 = normal
//     playback rate, 8192 = twice as fast). Volume is PSX attenuation:
//     0 = loudest, -10000 = silent; the stock rev channel sits around -5500
//     at full rev and fades toward -10000. To make it LOUDER, move the
//     volume toward 0: add a POSITIVE CD2_SND_*_BIAS and/or raise the gain
//     above 4096 (gain divides the remaining attenuation: 8192 ≈ twice as
//     loud). Clamps keep everything in [-10000, 0].
#define CD2_REV_RISE_SCALE    6144  // fp: rev rise slew multiplier (~1.5x stock)
#define CD2_REV_DROP_SCALE    4096  // fp: rev fall slew multiplier (1.0x stock)
#define CD2_SND_PITCH_SCALE   4096  // fp: rev+idle pitch multiplier (1.0)
#define CD2_SND_PITCH_BIAS    1024  // additive rev-channel pitch (faster spin-up)
#define CD2_SND_IDLE_PITCH_BIAS 512 // additive idle-channel pitch
#define CD2_SND_REV_GAIN      4096  // fp: rev loudness gain (>4096 = louder)
#define CD2_SND_REV_BIAS      1800  // rev volume bias toward 0 (louder)
#define CD2_SND_IDLE_GAIN     4096  // fp: idle loudness gain (>4096 = louder)
#define CD2_SND_IDLE_BIAS     1800  // idle volume bias toward 0 (louder)
#define CD2_SND_MIN_VOL      -10000  // clamp floor for the scaled volumes
#define CD2_SND_MAX_VOL      0      // clamp ceiling


// ==============================================================
// PER-VEHICLE SPREAD (weight/control)
// ==============================================================

// Per-vehicle variety references (typical values in this data set):
#define CD2_REF_PW          4096   // typical powerRatio/mass ratio (4096/4096)
#define CD2_REF_MASS        4096   // typical car mass (fixed-point scale)


// ==============================================================
// PRESETS / CONFIG / STATE
// ==============================================================

// presets
enum
{
	CD2_PRESET_DEFAULT = 0,   // TM2 baseline
	CD2_PRESET_TURBO = 1,     // faster + stiffer
	CD2_PRESET_DRIFTY = 2,    // looser + more agile
	CD2_PRESET_CUSTOM = 3,    // sliders were hand-tuned
	CD2_PRESET_COUNT = 4
};

// --------------------------- state ---------------------------------------

typedef struct CD2_STATS
{
	int topSpeed;      // speed-units/frame
	int reverseSpeed;  // speed-units/frame
	int accel;         // speed-units/frame²
	int brake;         // speed-units/frame²
	int drag;          // fixed point /frame
	int handling;      // yaw, PSX-units/frame
	int angularAccel;  // yaw, PSX-units/frame²
	int grip;          // fixed point /frame
	int control;       // steering/pivot authority, fixed point (4096 = average car)
} CD2_STATS;

// ---- roll-over suppression -------------------------------------------
// Tilt is the car's up axis . world up (4096 = upright, 0 = on its side).
// A car may lean up to CD2_ROLL_LIMIT_DEFAULT degrees (two wheels) but is
// stopped past that, and the pitch/roll rates are capped so a single impulse
// can't flip it in one frame.
#define CD2_ROLL_LIMIT_DEFAULT  60       // degrees (0 = off)
#define CD2_ROLL_MAX_AV         0x200000 // per-axis pitch/roll rate cap (raw)
#define CD2_ROLL_RECOVER_DEG    6        // post-physics upright correction per frame

// ---- damage multipliers (applied uniformly to EVERY car) -------------
// Percent of the stock damage that is actually applied, for the player car
// AND opponent cars alike (engine hook fires for every car). Opponents also
// use the player damage model for car-vs-car hits instead of the harsher
// traffic/civ multiplier (see cd2OnCarVsCar).
//   car-vs-car  : CD2_CAR_CAR_DAMAGE_DEFAULT  (67% of stock = 33% nerf)
//   car-vs-solid: CD2_SCENERY_DAMAGE_DEFAULT  (buildings/walls/objects)
// Runtime-tunable via the pause menu / [cainescrossfire] car_car_damage, scenery_damage.
#define CD2_CAR_CAR_DAMAGE_DEFAULT 67    // % of stock car-vs-car damage (10..100)
// Collision damage between a cainescrossfire car (player/opponent) and civilian
// traffic, as a percentage of what the pair would otherwise exchange.
// 20 = 80% off: traffic is there to be shoved and tumbled, not wrecked on
// contact.
#define CD2_CAR_TRAFFIC_DAMAGE		20

// Additional scenery-damage cut for traffic specifically, on top of
// scenery_damage. 50 = halved again. Traffic spends its life scraping
// walls, and the tumble is the point rather than the damage.
#define CD2_TRAFFIC_SCENERY_EXTRA	50

// Weapons hit traffic harder than they hit the module's own cars. 400 = 4x
// weapon damage, so a burst sweeps civilians out of the way instead of
// plinking at them - traffic exists to be collateral, not a damage sink.
// Only weapon damage is scaled here; explosion/scenery damage already has its
// own traffic handling in cd2OnDamageScale.
#define CD2_TRAFFIC_WPN_TAKEN	400

// Shoving traffic. A cainescrossfire car punting a civ car rolls it over, so the
// twist response to a collision is deliberately high - but capped, because
// past a point it stops reading as being barged aside and starts looking
// like a physics glitch.
#define CD2_TRAFFIC_ROLL_RATE	4			// roll impulse per unit of impact speed.
				// 300 saturated the cap on every
				// single shove (a normal impact term
				// is ~300000), so the roll never
				// varied with how hard it was hit.
#define CD2_TRAFFIC_ROLL_MAX	0x180000	// ceiling: brisk tumble, not a blur
#define CD2_TRAFFIC_SCRAPE_ROLL	0x300000	// ceiling for a ground scrape instead

#define CD2_SCENERY_DAMAGE_DEFAULT 65    // % of stock car-vs-solid damage (0..100)
#define CD2_AI_DAMAGE_TAKEN_DEFAULT 50   // % damage an opponent takes (10..400)

// ---- destroyed-car respawn -------------------------------------------
// A wrecked car the module owns (the player and the AI opponents) returns to
// the position it started the level at, after CD2_RESPAWN_DELAY frames.
// Spawn points come later; for now the start point is the respawn point.
// Respawn delay is FIXED at 5 seconds. NOTE the sim steps at 30 fps, NOT 60:
// State_GameLoop gates StepGame behind FilterFrameTime (2 vblanks) and the code
// comment says "always stay 30 FPS". CAR_STEP - and so the respawn tick - fires
// once per StepCars, i.e. once per 30 Hz step, so 5s == 150 frames. (It was
// 300 with a "300 frames at 60fps" comment, which is why respawn took ~10s.)
// Deliberately not configurable: every car, player or opponent, comes back on
// the same clock, and the old max/min knobs let it drift as low as half a
// second.
#define CD2_RESPAWN_DELAY 150
// Prototype opponent-AI behaviour states (also CD2_CONFIG.aiForceState; 0 = let
// the AI pick). See ai/opponent.c.
enum
{
	CD2_AI_AUTO = 0,	// decide from conditions
	CD2_AI_DISPERSE,	// opening move: break away from the spawn cluster
	CD2_AI_ROAM,		// cruise (road-guided) looking for a fight
	CD2_AI_ATTACK,		// engage the nearest target, on the move
	CD2_AI_FLEE,		// hurt, or outnumbered: break contact and run
	CD2_AI_RECOVER,		// heavily damaged: back off, stabilise
	CD2_AI_STATE_COUNT
};

// Opponent role archetypes (config ai_role; -1 = auto round-robin by car index).
// The role only changes what goal the navigator is pointed at; the state machine
// (HUNT/FLEE/EVADE/...) still overrides when it must.
enum
{
	CD2_AI_ROLE_CHASER = 0,		// straight at the player, guns hot
	CD2_AI_ROLE_FLANKER,		// sweep to the side of the player's travel
	CD2_AI_ROLE_AMBUSHER,		// run ahead of the player and lie in wait
	CD2_AI_ROLE_HARVESTER,		// roam for pickups/caches (rally points for now)
	CD2_AI_ROLE_COUNT
};

// ---- pursuit theme --------------------------------------------------------
// When set to 1, cainescrossfire forces the in-game music onto the "pursuit" segment
// of the current track -- the tune the game plays while cops chase you
// (FunkUpDaBGMTunez(1) -> Song_SetPos = xm_coptrackpos[current_music_id]).
// Set to 0 to leave the music to the stock cop/felony logic.
#ifndef CD2_ENFORCE_PURSUIT_MUSIC
#define CD2_ENFORCE_PURSUIT_MUSIC 1
#endif

typedef struct CD2_CONFIG
{
	int enabled;
	int topSpeed;
	int accel;
	int brake;         // peak brake decel (speed-units/frame²)
	int handling;
	int grip;
	int preset;        // CD2_PRESET_*
	int fovPull;       // 0..100
	int tightTurn;     // 0/1 master toggle
	int tightStrength; // 0..100 pivot authority
	int tightInput;    // CD2_TIGHT_INPUT_*
	int tmbButtons;    // 0/1: TMB in-car button layout (Square gas, Circle brake)
	int tmbTight;      // 0/1: which face button is Tight Turn (0=Cross/bottom, 1=Square/left)
	int debugLog;      // 0/1: log player-car input/velocity telemetry to REDRIVER2.log
	int allWeapons;    // 0/1: test grant - spawn with every weapon at max capacity

	// roll-over suppression: cars may tip onto two wheels but are stopped
	// from rolling past this tilt (degrees; 0 = off)
	int rollLimit;

	// missile presentation
	char missileModel[24]; // model name for the missile body ("" = line fallback)
	int missileScale;      // fixed point model scale (4096 = 1x)
	int missileSound;      // SOUND_BANK_SFX sample played on missile launch

	int sceneryDamage;     // % of stock damage a car takes hitting solid scenery/objects
	int carCarDamage;      // % of stock damage applied to car-vs-car hits
	int aiDamageTaken;     // % damage an opponent takes (they were dying too fast)
	int respawn;           // 0/1: destroyed cars return to their start point
	int respawnDelay;      // frames a destroyed car stays out (default ~5s)
	int navDebug;          // 0/1: draw the navigation graph (nodes/edges/routes)
	// How many AI opponents this MATCH fields, 0..CD2_AI_MAX. Defaults to 0:
	// opponents are opted into per match (ai_opponents in the ini, the pause menu,
	// or CC_OPPONENTS for a harness run), never spawned just because the module
	// is loaded.
	int aiOpponents;
	int aiForceState;      // CD2_AI_AUTO (0) or a forced CD2_AI_* behaviour
	int aiDebug;           // 0/1: draw the AI internal-value readout on screen
	int aiRole;            // CD2_AI_ROLE_* (-1 = auto round-robin by car index)

	// ---- factions (factions/, see FACTIONS.md) ---------------------------
	// 0/1: give every car a faction identity (the five-row registry) and name
	// it — in its own colour — in messages. Off restores the anonymous
	// role-name wording ("Flanker was killed by ...").
	int factions;
	// the player's faction, a CD2_FAC_* index (0 = TANNER). A faction that
	// does not compete (Caine, the host) is refused: the player drives.
	int playerFaction;

	// ---- team palettes (factions/, see the SDK's ped-palette.md) ----------
	// 0/1: give each Tanner the OUTFIT colour of its faction, so a team reads at
	// a glance. Per-instance - only that ped's CLUT rows are recoloured; everyone
	// else keeps stock colours. Needs `factions` on.
	int teamPalette;
	// how far each palette entry moves toward the faction colour, 0..256
	int teamPaletteStrength;
	// how far the dark end of the outfit is lifted, 0..31 (0 = keep the source
	// brightness and let a dark suit stay dark, 31 = flat)
	int teamPaletteFloor;
} CD2_CONFIG;

typedef struct CD2_CAR
{
	int yawRate;       // current yaw rate, PSX-units/frame (signed)
	int slip;          // lateral velocity, speed units (signed), for visuals
	int roll;          // smoothed body roll, PSX angle units
	int pitch;         // turbo kick: nose-up pitch (the far wheels lifting)
	int throttle;      // +1/-1/0 raw throttle captured at CAR_STEP (see note)
	int pivotDir;      // latched tight-turn direction +1/-1/0
	int slideTicks;    // recovery frames remaining after a slide ends
	int aiPivot;       // AI-requested tight-turn direction +1/-1/0 (0 = none)
} CD2_CAR;

extern CD2_CONFIG gCd2Cfg;

// How many opponents THIS match fields: the ai_opponents match setting, unless the
// CC_OPPONENTS environment overrides it for a headless run. Always clamped to
// the slots available (0..CD2_AI_MAX).
extern int cd2MatchOpponents(void);

// Exported for the presentation source file (cainescrossfiremedia.c) of this merged
// module: the effective top speed of a car (fixed-point speed-units/frame).
int cd2CarTopSpeed(void* cp);
int cd2CarBrake(void* cp);		// speed-units/frame^2

// Exported for the core + weapons files: 1 when a car is past the damage cap
// (totaled wreck — no driving input, no weapons).
// scale a damage value by a percentage (shared with the weapon core)
int cd2ScaleDamage(int value, int pct);

// scenery impacts taken by `car` this level (observability for the AI)
int cd2SceneryHits(void* car);

// 1 for stock civ traffic: not a car the module drives (see cd2IsTraffic).
int cd2IsTraffic(CAR_DATA* cp);

int cd2CarTotaled(void* cp);

// Exported for the AI (ai/): request (dir = +1/-1) or clear (0) an acute
// in-place "tight turn" pivot for a car this frame.
void cd2CarSetAiPivot(void* cp, int dir);

#endif /* CAINESCROSSFIRE_H */
