// cainescrossfirewreckfx.c — Combat D2: the wreck edge (explosion + kill credit).
//
// (Split out of the old cainescrossfirecombat.c; see cainescrossfire_internal.h for the file
// map.)
//
// Edge-detects a car reaching the game's damage cap ("totaled") and, once, at
// the moment it crosses the threshold: spawns a harmless BIG_BANG barrage on
// the wreck profile, tosses the wreck so it tumbles and comes to rest, and
// posts the death message (credited to the last weapon that hit it). Also owns
// cd2CarTotaled - the canonical "past the cap" test the rest of the module and
// the weapons layer share.
//
// The damage cap is the one the stock game uses (cars.c DrawCar):
// MaxPlayerDamage[padid] for the player, otherwise MaxPlayerDamage[0].

#include "driver2.h"
#include "cainescrossfire.h"
#include "cainescrossfire_internal.h"
#include "cars.h"
#include "debris.h"		/* SMOKE_BLACK / SMOKE_GREY - the damage smoke types */
#include "job_fx.h"
#include "mission.h"
#include "players.h"
#include "convert.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_hud.h"	/* HUD messages (kill credit) */
#include "factions/factions.h"	/* the five teams: the named colours */
#include "weapons/fx/fx.h"
#include "weapons/core/weapon_internal.h"	/* cd2WpnTakeAttacker */
#include "ai/ai.h"	/* cd2AiIsOpponent / cd2AiRoleOf / cd2AiRoleNameOf */

// per-car latch: 1 once the car has crossed the damage cap (edge detection)
static char gWasTotaled[MAX_CARS];

// wreck toss (applied once, on the explosion edge)
// Parenthesised: these are multiplications, and an unparenthesised macro body
// silently reparenthesises when it is used in a wider expression.
#define CD2C_TUMBLE_LAUNCH   (0x18000 * 5)  // upward velocity impulse (raw)
#define CD2C_TUMBLE_SPIN     (0x100000 * 50) // roll/pitch angular impulse range (raw)

// The canonical "totaled" cap, mirroring cars.c DrawCar.
static int cd2cMaxDamage(CAR_DATA* cp)
{
	int maxDamage = MaxPlayerDamage[0];

	if (cp->controlType == CONTROL_TYPE_PLAYER && cp->ai.padid != NULL &&
		*cp->ai.padid >= 0 && *cp->ai.padid < 2)
		maxDamage = MaxPlayerDamage[*cp->ai.padid];

	return maxDamage;
}

// Exported for the core (cainescrossfire.c) + weapons files: is this car past the
// damage cap (totaled)? Mirrors the cap cars.c DrawCar uses.
int cd2CarTotaled(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	return cp->totalDamage >= cd2cMaxDamage(cp);
}

// The car's damage cap (the same value cd2CarTotaled tests against), so a caller
// can turn totalDamage into a 0..1 health fraction - the lock-on health bar.
int cd2CarMaxDamage(void* vcp)
{
	return cd2cMaxDamage((CAR_DATA*)vcp);
}

// ---------------------------------------------------------------------------
// The damage ladder - JER_EVENT_CAR_DAMAGE_FX (cars.c, DrawCar)
// ---------------------------------------------------------------------------
// The engine's damage smoke keys off per-zone ap.damage, and its fire off "past the cap
// AND nearly stopped". Neither is a health ladder: a car can be most of the way to a
// write-off and emit nothing, or take one engine-zone hit and smoke like a chimney. This
// handler answers the engine with the ladder the player can actually read off the HUD:
//
//   above CD2_DMG_HEALTH_SMOKE      nothing
//   at or below it                  grey smoke
//   at or below CD2_DMG_HEALTH_FIRE on fire, with black smoke
//   past the damage cap             the burning WRECK: the big fire + thick black smoke
//
// `health` arrives from the engine already normalised against the car's own cap (the
// per-pad one for a player car), which is the same number the lock-on bar computes, so
// the thresholds need no arithmetic here. The engine still applies its own speed gates,
// so smoke needs the car under ~98 speed units and the fire under ~7: a wreck lights up
// as it comes to rest, and a car at speed cannot flood the shared particle pool.
//
// `handled = 1` in every case: this module owns the decision for every car, players and
// traffic alike, so a car that answers "nothing" suppresses the stock smoke too - which
// is the point, since 50% health has to mean quiet for the stock 2000-zone-damage puff
// as well.
static signed char gDmgFx[MAX_CARS];	// 0 nothing, 1 smoking, 2 burning, 3 wreck

// [D] [T]
static int cd2cOnDamageFx(void* ud, void* args)
{
	JER_ARGS_CAR_DAMAGE_FX* a = (JER_ARGS_CAR_DAMAGE_FX*)args;
	CAR_DATA* cp;
	int step;

	(void)ud;

	if (a == NULL || a->car == NULL)
		return JER_RESULT_CONTINUE;

	cp = (CAR_DATA*)a->car;

	if (cd2CarTotaled(cp))
	{
		step = 3;

		a->smokeType = SMOKE_BLACK;
		a->smokeStart = CD2_WRECK_SMOKE_START;
		a->smokeEnd = CD2_WRECK_SMOKE_END;
		a->flame = 1;
		a->flameStart = CD2_WRECK_FIRE_START;
		a->flameEnd = CD2_WRECK_FIRE_END;
	}
	else if (a->health <= CD2_DMG_HEALTH_FIRE)
	{
		step = 2;

		a->smokeType = SMOKE_BLACK;
		a->smokeStart = CD2_DMG_SMOKE_START;
		a->smokeEnd = CD2_DMG_SMOKE_END;
		a->flame = 1;
		a->flameStart = CD2_DMG_FIRE_START;
		a->flameEnd = CD2_DMG_FIRE_END;
	}
	else if (a->health <= CD2_DMG_HEALTH_SMOKE)
	{
		step = 1;

		a->smokeType = SMOKE_GREY;
		a->smokeStart = CD2_DMG_SMOKE_START;
		a->smokeEnd = CD2_DMG_SMOKE_END;
		a->flame = 0;
	}
	else
	{
		step = 0;

		a->smokeType = 0;
		a->flame = 0;
	}

	a->handled = 1;

	// the ladder's trace: one line per CHANGE, which is what makes the two thresholds
	// checkable from a log instead of only by eye - a run that kills a car must show
	// hp falling through 50 (grey smoke) and 25 (fire) before the wreck line
	if (cp->id >= 0 && cp->id < MAX_CARS && gDmgFx[cp->id] != (signed char)step)
	{
		gDmgFx[cp->id] = (signed char)step;

		if (gCd2Cfg.debugLog)
			jer_log("[cainescrossfire] damage fx car=%d hp=%d%% -> %s\n", cp->id, a->health,
				(step == 3) ? "WRECK: big fire + thick smoke"
				: (step == 2) ? "on fire + black smoke"
				: (step == 1) ? "grey smoke"
				: "nothing");
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// The death banner: naming a car
// ---------------------------------------------------------------------------
// A car is announced as its FACTION (the registry in factions/), drawn in that
// faction's colour, because that is the identity the tournament gives it. The
// local player is kept as "You" - it is the player reading the line - but still
// carries its faction's colour.
//
// A faction can field more than one car (VASQUEZ fields two), so the second and
// later get an index: "VASQUEZ 2 was killed by MCKENZIE" rather than an
// ambiguous tag for both. With no faction at all (factions off in config, or a
// car the roster never saw) the old anonymous wording is kept, in a neutral
// grey, so a message is never dropped for lack of a team.
#define CD2C_NONAME_R	200
#define CD2C_NONAME_G	200
#define CD2C_NONAME_B	200

// The name of `cp` for a message, written into `buf` when it needs assembling.
// Returns the name (a literal, a faction tag, or `buf`) and fills the colour.
static const char* cd2cAnnounce(CAR_DATA* cp, const char* fallback, char* buf, int bufLen,
	unsigned char* r, unsigned char* g, unsigned char* b)
{
	int faction = cd2FacOfCar(cp);
	int i, index = 0, total = 0;

	if (faction == CD2_FAC_NONE || !cd2FacColourOf(faction, r, g, b))
	{
		*r = CD2C_NONAME_R;
		*g = CD2C_NONAME_G;
		*b = CD2C_NONAME_B;

		return fallback;
	}

	if (cp->id == MainPlayer.playerCarId)
		return "You";

	// number the cars of a faction that fields more than one
	for (i = 0; i < MAX_CARS; i++)
	{
		if (cd2FacOfCarId(i) != faction)
			continue;

		total++;

		if (i == cp->id)
			index = total;
	}

	if (total > 1)
		snprintf(buf, bufLen, "%s %d", cd2FacTagOf(faction), index);
	else
		snprintf(buf, bufLen, "%s", cd2FacTagOf(faction));

	return buf;
}

// The two run kinds of a banner: the connective words (ambient colour, so the
// line still matches every other HUD message) and a NAME (its faction colour).
static void cd2cSegWord(JER_HUD_SEG* segs, int* n, const char* text)
{
	segs[*n].text = text;
	segs[*n].r = 0;
	segs[*n].g = 0;
	segs[*n].b = 0;
	segs[*n].ambient = 1;
	(*n)++;
}

static void cd2cSegName(JER_HUD_SEG* segs, int* n, const char* text,
	unsigned char r, unsigned char g, unsigned char b)
{
	segs[*n].text = text;
	segs[*n].r = r;
	segs[*n].g = g;
	segs[*n].b = b;
	segs[*n].ambient = 0;
	(*n)++;
}

// CAR_STEP: explode once when a car first reaches the damage cap.
static int cd2cOnCarStep(void* ud, void* args)
{
	JER_ARGS_CAR_STEP* a = (JER_ARGS_CAR_STEP*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	(void)ud;

	if (cp->id < 0 || cp->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	if (cd2CarTotaled(cp))
	{
		if (!gWasTotaled[cp->id])
		{
			VECTOR blastPos;

			gWasTotaled[cp->id] = 1;
			blastPos.vx = cp->hd.where.t[0];
			blastPos.vy = cp->hd.where.t[1] + 1;
			blastPos.vz = cp->hd.where.t[2];

			// spectacular but HARMLESS death blast: a short burst of big
			// explosions on the CD2_FX_WRECK profile (collide = 0). radius and
			// damage are 0, so it does no radial damage either — the car dies
			// in flames without the blast hurting or shoving anything around it.
			cd2FxBarrage(&blastPos, NULL, CD2_FX_WRECK, 3, 3, 120, 0, 0, NULL, NULL);

			// toss the wreck so it tumbles and comes to rest: upward pop +
			// random roll/pitch spin (yaw is re-owned by the handling module,
			// but roll/pitch are free to tumble)
			cp->st.n.linearVelocity[1] += CD2C_TUMBLE_LAUNCH;
			cp->st.n.angularVelocity[0] += (Random2(CD2C_TUMBLE_SPIN) - (CD2C_TUMBLE_SPIN >> 1));
			cp->st.n.angularVelocity[2] += (Random2(CD2C_TUMBLE_SPIN) - (CD2C_TUMBLE_SPIN >> 1));

			// Death banner. The killer is the last weapon to hit this car
			// (cd2WpnTakeAttacker clears the record, so a death is never
			// credited to a stale hit); no record means nothing to blame -
			// scenery, or the car doing it to itself.
			//
			//   killed by the local player  -> "You killed <victim>"
			//   killed by an enemy, us      -> "You were killed by <killer>"
			//   killed by an enemy, an npc  -> "<victim> was killed by <killer>"
			//   nobody to blame / self      -> "<victim> died"
			//
			// The names are drawn in their faction's colour (factions/); the
			// wiring words stay ambient so the line still looks like every
			// other HUD message. Only the local player and our opponents get a
			// line: announcing every civ car that scrapes a wall would bury
			// the screen.
			{
				int killerId = cd2WpnTakeAttacker(cp->id);
				int isPlayer = (cp->controlType == CONTROL_TYPE_PLAYER);
				int isNpc = cd2AiIsOpponent(cp);

				if (isPlayer || isNpc)
				{
					int isLocal = (cp->id == MainPlayer.playerCarId);
					int byPlayer = (killerId >= 0 && killerId == MainPlayer.playerCarId);
					const char* role = isNpc ? cd2AiRoleNameOf(cd2AiRoleOf(cp)) : NULL;
					const char* victim;
					const char* killer = NULL;
					char vBuf[24], kBuf[24];
					unsigned char vr, vg, vb, kr = 0, kg = 0, kb = 0;
					JER_HUD_SEG segs[5];
					int n = 0;

					// an enemy is any other car we can name: another opponent,
					// or a second player. Anything else (traffic, an unknown
					// slot) stays unattributed rather than guessed at.
					if (killerId >= 0 && killerId < MAX_CARS && killerId != cp->id && !byPlayer)
					{
						CAR_DATA* kcp = &car_data[killerId];
						const char* kFallback = NULL;

						if (cd2AiIsOpponent(kcp))
							kFallback = cd2AiRoleNameOf(cd2AiRoleOf(kcp));
						else if (kcp->controlType == CONTROL_TYPE_PLAYER)
							kFallback = "the other player";

						if (kFallback != NULL)
							killer = cd2cAnnounce(kcp, kFallback, kBuf, sizeof(kBuf), &kr, &kg, &kb);
					}

					victim = cd2cAnnounce(cp, isLocal ? "You" : ((role != NULL) ? role : "The player"),
						vBuf, sizeof(vBuf), &vr, &vg, &vb);

					if (byPlayer)
					{
						cd2cSegWord(segs, &n, "You killed ");
						cd2cSegName(segs, &n, victim, vr, vg, vb);
					}
					else if (killer != NULL && isLocal)
					{
						cd2cSegWord(segs, &n, "You were killed by ");
						cd2cSegName(segs, &n, killer, kr, kg, kb);
					}
					else if (killer != NULL)
					{
						cd2cSegName(segs, &n, victim, vr, vg, vb);
						cd2cSegWord(segs, &n, " was killed by ");
						cd2cSegName(segs, &n, killer, kr, kg, kb);
					}
					else
					{
						cd2cSegName(segs, &n, victim, vr, vg, vb);
						cd2cSegWord(segs, &n, " died");
					}

					if (gCd2Cfg.debugLog)
						printInfo("[cainescrossfire] death: car=%d victim=%s killer=%d(%s) playerCar=%d\n",
							cp->id, victim, killerId, (killer != NULL) ? killer : "-",
							(int)MainPlayer.playerCarId);

					// one banner at a time, like the plaintext replace it
					// supersedes: a frame with several deaths must not stack
					jer_hud_clear();
					jer_hud_message_segs(segs, n, 0);	// 0 = the default ~3s
				}
			}
		}

		// dead car: lock it — no throttle/steer/brake, handbrake on so the
		// wreck rolls to a stop and can't be driven (weapons gate on this too)
		cp->thrust = 0;
		cp->wheel_angle = 0;
		cp->handbrake = 1;
	}
	else
	{
		gWasTotaled[cp->id] = 0;
	}

	return JER_RESULT_CONTINUE;
}

// RESET_CAR: clear the latch so a respawned car can explode again.
static int cd2cOnResetCar(void* ud, void* args)
{
	JER_ARGS_RESET_CAR* a = (JER_ARGS_RESET_CAR*)args;
	(void)ud;

	if (a->carId >= 0 && a->carId < MAX_CARS)
	{
		gWasTotaled[a->carId] = 0;
		gDmgFx[a->carId] = 0;		// the ladder starts silent again on a respawn
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Registration (called once by jer_module_cainescrossfire_entry in cainescrossfire.c)
// ---------------------------------------------------------------------------
void cd2WreckFxRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_STEP, cd2cOnCarStep, NULL, -1); // runs before cainescrossfire.c's CAR_STEP
	ctx->jer_register_hook(ctx, JER_EVENT_RESET_CAR, cd2cOnResetCar, NULL, 0);

	// the damage ladder: the engine asks before it emits a car's damage smoke or fire
	// (cars.c DrawCar) and this answers with health-based thresholds - see
	// cd2cOnDamageFx, and CD2_DMG_* in cainescrossfire.h
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DAMAGE_FX, cd2cOnDamageFx, NULL, 0);

	ctx->jer_log(ctx, "[cainescrossfire] wreck effects registered (SDK v%d)\n", ctx->sdkVersion);
}
