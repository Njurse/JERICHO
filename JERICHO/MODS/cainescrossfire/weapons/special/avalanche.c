// weapons/special/avalanche.c — AVALANCHE's special: MONSTER CRUSH.
//
// First press: a five-second surge — max normal speed and raised grip — so the
// truck can bulldoze. During the surge, ramming another car makes Avalanche
// CLIMB ON TOP of it: the attacker is held above the victim, the victim's
// velocity is pinned (it is trapped), the attacker's tyres spin and it can
// still be steered, and after the hold it shoves the victim off and drops back
// down.
//
// First cut: the crush directly holds the attacker's position over the victim
// and zeroes the victim's velocity for the hold, with a rocking pitch; a rigged
// climb animation is a later refinement.

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "sound.h"
#include "gamesnd.h"
#include "jericho.h"
#include "jer_events.h"
#include "cainescrossfire.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "weapons/special/special.h"

#include <string.h>

#define CD2_AVALANCHE_FRAMES	150	// 5s boost
#define CD2_AVALANCHE_GRIP	6144	// fp grip multiplier while surging (1.5x)
#define CD2_AVALANCHE_THRUST	6400	// thrust while surging
#define CD2_CRUSH_FRAMES	55	// 1.5s hold on top of a victim
#define CD2_CRUSH_LIFT		0	// extra height for the attacker over the victim's origin
#define CD2_CRUSH_ROCK		0x0A000	// rocking pitch added each frame (raw avel)
#define CD2_CRUSH_PUSH		260	// push-off speed given to the victim at the end
#define CD2_CRUSH_CRUNCH_EVERY	12	// frames between crunch noises (the engine's heavy crash sample)

static int gAvalancheFrames[MAX_CARS];	// boost frames left
static int gCrushFrames[MAX_CARS];	// crush frames left (0 = not crushing)
static int gCrushVictim[MAX_CARS];	// the car being crushed (-1)
static int gCrushFloor[MAX_CARS];	// the victim's ride height, captured at crush start
static int gCrushNoise[MAX_CARS];	// frames until the next crunch
static int gAvalancheChannel = -1;

static void cd2AvalancheFire(void* vcp)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL || cp->ap.carCos == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	gAvalancheFrames[cp->id] = CD2_AVALANCHE_FRAMES;

	if (gAvalancheChannel < 0)
	{
		gAvalancheChannel = GetFreeChannel(1);
		LockChannel(gAvalancheChannel);
	}

	if (gAvalancheChannel >= 0)
		Start3DSoundVolPitch(gAvalancheChannel, SOUND_BANK_SFX, 6,
			cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], -2000, 4096 - 900);
}

// Higher grip and thrust while the surge runs.
static int cd2AvalancheOnFriction(void* ud, void* args)
{
	JER_ARGS_CAR_FRICTION* a = (JER_ARGS_CAR_FRICTION*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;

	(void)ud;

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS || gAvalancheFrames[cp->id] <= 0)
		return JER_RESULT_CONTINUE;

	a->frontFS = (int)(((long long)a->frontFS * CD2_AVALANCHE_GRIP) >> 12);
	a->rearFS = (int)(((long long)a->rearFS * CD2_AVALANCHE_GRIP) >> 12);

	return JER_RESULT_CONTINUE;
}

static int cd2AvalancheOnEngine(void* ud, void* args)
{
	JER_ARGS_CAR_ENGINE* a = (JER_ARGS_CAR_ENGINE*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;

	(void)ud;

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS || gAvalancheFrames[cp->id] <= 0)
		return JER_RESULT_CONTINUE;

	if (a->thrust > 0)
		a->thrust = CD2_AVALANCHE_THRUST;

	return JER_RESULT_CONTINUE;
}

// A surging Avalanche that touches a car latches onto it.
static int cd2AvalancheOnCollision(void* ud, void* args)
{
	JER_ARGS_COLLISION* a = (JER_ARGS_COLLISION*)args;
	CAR_DATA* c0 = (CAR_DATA*)a->car0;
	CAR_DATA* c1 = (CAR_DATA*)a->car1;
	CAR_DATA *att = NULL, *vic = NULL;

	(void)ud;

	if (c0 == NULL || c1 == NULL)
		return JER_RESULT_CONTINUE;

	// figure out which (if either) is a surging Avalanche, and who it hit
	if (c0->id >= 0 && c0->id < MAX_CARS && gAvalancheFrames[c0->id] > 0 && gCrushFrames[c0->id] <= 0)
	{
		att = c0; vic = c1;
	}
	else if (c1->id >= 0 && c1->id < MAX_CARS && gAvalancheFrames[c1->id] > 0 && gCrushFrames[c1->id] <= 0)
	{
		att = c1; vic = c0;
	}

	if (att == NULL || vic->id < 0 || vic->id >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	// both must be real cars
	if (att->controlType == CONTROL_TYPE_NONE || vic->controlType == CONTROL_TYPE_NONE)
		return JER_RESULT_CONTINUE;

	gCrushFrames[att->id] = CD2_CRUSH_FRAMES;
	gCrushVictim[att->id] = vic->id;
	gCrushFloor[att->id] = vic->hd.where.t[1];	// its ride height, to hold it flat
	gCrushNoise[att->id] = 0;

	// and the victim stops taking the crash: the crush handles the hold
	return JER_RESULT_CONTINUE;
}

static int cd2AvalancheOnFrame(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];

		if (gAvalancheFrames[i] > 0)
		{
			if (cp->controlType == CONTROL_TYPE_NONE || cp->ap.carCos == NULL)
				gAvalancheFrames[i] = 0;
			else if (--gAvalancheFrames[i] <= 0)
				gAvalancheFrames[i] = 0;
		}

		if (gCrushFrames[i] <= 0)
			continue;

		{
			int v = gCrushVictim[i];

			if (v < 0 || v >= MAX_CARS || car_data[v].controlType == CONTROL_TYPE_NONE ||
			    cp->controlType == CONTROL_TYPE_NONE || cp->ap.carCos == NULL)
			{
				gCrushFrames[i] = 0;
				gCrushVictim[i] = -1;
				continue;
			}
			else
			{
				CAR_DATA* vic = &car_data[v];

				// hold the attacker ON the victim. The position is written before the
				// physics step, so a lift is not where the truck ends up - it is how much
				// the solver has to push back, and a positive one just floated it. Level
				// with the victim's own origin is the "sitting on it" baseline.
				cp->hd.where.t[0] = vic->hd.where.t[0];
				cp->hd.where.t[2] = vic->hd.where.t[2];
				cp->hd.where.t[1] = vic->hd.where.t[1] + CD2_CRUSH_LIFT;

				// trap the victim: pin its velocity so it cannot drive away - and pin it
				// FLAT on the road. The press would otherwise shove it down through the
				// surface and leave it rocking, so its ride height is held at what it was
				// before the crush and its tumble is stopped: bottom along the road, level.
				vic->st.n.linearVelocity[0] = 0;
				vic->st.n.linearVelocity[1] = 0;
				vic->st.n.linearVelocity[2] = 0;
				vic->st.n.angularVelocity[0] = 0;
				vic->st.n.angularVelocity[1] = 0;
				vic->st.n.angularVelocity[2] = 0;
				vic->hd.where.t[1] = gCrushFloor[i];
				vic->hd.speed = 0;

				// spin the attacker's tyres (visual) and rock it
				cp->wheelspin = 1;
				cp->st.n.angularVelocity[0] += CD2_CRUSH_ROCK;

				// crunching: the engine's own heavy-crash sample, over and over
				if (--gCrushNoise[i] <= 0)
				{
					int chan = GetFreeChannel(1);

					gCrushNoise[i] = CD2_CRUSH_CRUNCH_EVERY;

					if (chan >= 0)
						Start3DSoundVolPitch(chan, SOUND_BANK_SFX, 6,
							vic->hd.where.t[0], vic->hd.where.t[1], vic->hd.where.t[2],
							-2200, 3072 + (rand() % 1024));
				}

				if (--gCrushFrames[i] <= 0)
				{
					// shove the victim off along the attacker's heading
					VECTOR fwd;

					cd2WpnForward(cp, &fwd);

					vic->st.n.linearVelocity[0] += (int)(((long long)fwd.vx * CD2_CRUSH_PUSH) >> 12);
					vic->st.n.linearVelocity[2] += (int)(((long long)fwd.vz * CD2_CRUSH_PUSH) >> 12);
					vic->hd.speed = CD2_CRUSH_PUSH;

					gCrushVictim[i] = -1;
				}
			}
		}
	}

	return JER_RESULT_CONTINUE;
}

static int cd2AvalancheOnGameStart(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		gAvalancheFrames[i] = 0;
		gCrushFrames[i] = 0;
		gCrushVictim[i] = -1;
		gCrushFloor[i] = 0;
		gCrushNoise[i] = 0;
	}

	return JER_RESULT_CONTINUE;
}

void cd2SpecialAvalancheRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2AvalancheOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_FRICTION, cd2AvalancheOnFriction, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_ENGINE, cd2AvalancheOnEngine, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_COLLISION, cd2AvalancheOnCollision, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2AvalancheOnGameStart, NULL, 0);
	// the same reset when the game returns to the frontend menus
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_ENTERED, cd2AvalancheOnGameStart, NULL, 0);
}

static CD2_WEAPON_DEF cd2MakeAvalancheDef(void)
{
	CD2_WEAPON_DEF d;

	memset(&d, 0, sizeof(d));

	d.id = CD2_WID_SPECIAL_AVALANCHE;
	d.name = "special_avalanche";
	d.displayName = "Monster Crush";
	d.cls = CD2_WCLS_AOE;

	d.isSpecial = 1;

	d.maxAmmo = 2;			// profile capacity
	d.fireInterval = 30;
	d.refireCooldown = 900;		// profile recharge: 30s

	d.damage = 0;			// the crush/grip is the payload
	d.speed = 0;
	d.range = 0;
	d.life = CD2_AVALANCHE_FRAMES;

	d.explosionEffect = LITTLE_BANG;

	d.colR = 120; d.colG = 120; d.colB = 140;	// cold steel

	d.fire = cd2AvalancheFire;
	return d;
}

const CD2_WEAPON_DEF cd2WdefSpecialAvalanche = cd2MakeAvalancheDef();
