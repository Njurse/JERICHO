/* d2pl_health.c -- health for the player and every jer NPC, the aim-target
 * readout, deaths/respawns, and the debug test ped.
 *
 *   Player health   tracked through jer_npc_health on the local MOVEMENT ped
 *                   (gD2plPlayerPed0) and drawn by the engine as a half-height
 *                   bar under the damage bar (jer_hud_player_health).
 *   Target readout  while aiming at a ped: a placeholder name top centre and a
 *                   half-height health meter under it (jer_hud panels 4 + 5).
 *                   Colour is the damage bar's inverted: full = green, sliding
 *                   to red, flashing red in the last ~30%.
 *   Deaths          "<name> died" goes to the jer console. The player and the
 *                   test ped also get "You died." on the HUD; after
 *                   D2PL_RESPAWN_TICKS the player is refilled in place, the
 *                   test ped is respawned beside the player, and any other
 *                   ped is refilled.
 *   Test ped        a Tanner-model JMPPed (jer_npc_spawn_model(0, ..)) driven
 *                   here every tick. STAND: idles facing you. FOLLOW: keeps
 *                   close. ATTACK: carries the pistol, wanders around you while
 *                   in range, stops to aim and shoots; out of range it runs in.
 *                   Pause > JERICHO Addons > D2PL > Test ped, or
 *                   D2PL_TEST_PED=stand|follow|attack to spawn one on foot.
 */

#include "d2pl.h"
#include "jer_npc.h"
#include "jer_hud.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D2PL_PLAYER_HEALTH	100
#define D2PL_RESPAWN_TICKS	90		/* ~3 s at the 30 Hz tick */

#define D2PL_PANEL_NAME		4		/* jer_hud panel slots (cainescrossfire owns 0, 1, 3) */
#define D2PL_PANEL_BAR		5
#define D2PL_TARGET_RADIUS	120
#define D2PL_TARGET_RANGE	8000
#define D2PL_TARGET_HOLD	15		/* ticks the readout lingers after the aim leaves */

/* the engine walks a ped along dir - 2048 (see d2pl.h), so a ped that should
 * move toward heading h gets dir h + D2PL_NPC_DIR_OFS. Flip to 0 if the test
 * ped walks away from where it is going. */
#ifndef D2PL_NPC_DIR_OFS
#define D2PL_NPC_DIR_OFS	2048
#endif

#define D2PL_TP_SPAWN_DIST	500
#define D2PL_TP_FOLLOW_GAP	350
#define D2PL_TP_WALK		14
#define D2PL_TP_RUN			D2PL_RUN_SPEED
#define D2PL_TP_FIRE_RANGE	3000	/* shoots inside this, pursues outside */
#define D2PL_TP_WANDER_MIN	600		/* wander points ring the player at this.. */
#define D2PL_TP_WANDER_MAX	1400	/* ..to this */
#define D2PL_TP_AIM_TICKS	10		/* stand and aim this long before a shot */
#define D2PL_TP_DAMAGE		10
#define D2PL_TP_CHEST		60		/* render y above the ped origin for shots */

static const char* gBehaviourNames[D2PL_TP_COUNT] = { "STAND", "FOLLOW", "ATTACK" };

typedef struct D2plTestPed
{
	JerNpc* n;
	int behaviour;
	int dead;			/* ticks until the respawn, 0 = alive */
	int wander[2];
	int wanderTicks;
	int cooldown;
	int aim;
	int tracer;			/* ticks the last shot's tracer stays drawn */
	int tracerFrom[3];
	int tracerTo[3];
} D2plTestPed;

static D2plTestPed gTP = { 0, D2PL_TP_STAND, 0 };

static int gPlayerDead = 0;		/* ticks until the player refills, 0 = alive */
static void* gPlayerHealthPed = 0;

#define D2PL_PENDING_MAX	8
static void* gPending[D2PL_PENDING_MAX];	/* downed peds waiting for a refill */
static int gPendingTicks[D2PL_PENDING_MAX];

static void* gTarget = 0;
static int gTargetHold = 0;
static int gPanelsOwned = 0;
static int gEnvChecked = 0;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static const JER_HOST_API* Api(void)
{
	return jerHostApiGet();
}

static void Console(const char* text)
{
	const JER_HOST_API* api = Api();

	if (api != 0 && api->ui != 0 && api->ui->console_log != 0)
		api->ui->console_log("%s", text);
}

static void Hud(const char* text, int frames)
{
	const JER_HOST_API* api = Api();

	if (api != 0 && api->ui != 0 && api->ui->hud_message != 0)
		api->ui->hud_message(text, frames);
}

static int Frame(void)
{
	const JER_HOST_API* api = Api();

	if (api != 0 && api->game != 0 && api->game->frame != 0)
		return api->game->frame();

	return 0;
}

static int Dist2D(const int a[3], const int b[3])
{
	float dx = (float)(b[0] - a[0]);
	float dz = (float)(b[2] - a[2]);

	return (int)sqrtf(dx * dx + dz * dz);
}

static int HeadingTo(const int from[3], const int to[3])
{
	return D2plAtan2(to[0] - from[0], to[2] - from[2]);
}

static int PlayerPos(int out[3])
{
	return gD2plPlayerPed0 != 0 && jer_npc_get_pos((JerNpc*)gD2plPlayerPed0, out);
}

const char* D2plPedName(void* ped, char* buf, int max)
{
	if (ped == 0)
		snprintf(buf, max, "Nobody");
	else if (ped == gD2plPlayerPed0)
		snprintf(buf, max, "Player");
	else if (gTP.n != 0 && ped == (void*)gTP.n)
		snprintf(buf, max, "Test Tanner");
	else if (jer_npc_owned(ped))
		snprintf(buf, max, "JMPPed %02d", (int)(((size_t)ped >> 4) % 100));
	else
		snprintf(buf, max, "Pedestrian %02d", (int)(((size_t)ped >> 4) % 100));

	return buf;
}

/* the damage bar's colouring inverted: green full, red low, and below ~30%
 * a flash between dark and bright red (the stock bars' 16-frame flash) */
void D2plHealthColour(int value, int max, int* r, int* g, int* b)
{
	int f = max > 0 ? value * 4096 / max : 0;

	if (f < 0)
		f = 0;
	if (f > 4096)
		f = 4096;

	if (f <= 1229)
	{
		int t = Frame() % 16;

		if (t > 7)
			t = 16 - t;

		*r = 90 + (255 - 90) * t / 8;
		*g = 0;
		*b = 0;
	}
	else
	{
		int t = (f - 1229) * 4096 / (4096 - 1229);

		*r = 255 * (4096 - t) / 4096;
		*g = 255 * t / 4096;
		*b = 0;
	}
}

/* ------------------------------------------------------------------ */
/* deaths                                                              */
/* ------------------------------------------------------------------ */

int D2plPlayerDead(void)
{
	return gPlayerDead > 0;
}

/* called once, when a ped's health first reaches 0 */
void D2plOnPedKilled(void* ped)
{
	char name[32];
	char line[64];
	int i;

	D2plPedName(ped, name, sizeof(name));
	snprintf(line, sizeof(line), "%s died", name);
	Console(line);

	if (ped == gD2plPlayerPed0)
	{
		gPlayerDead = D2PL_RESPAWN_TICKS;
		Hud("You died.", D2PL_RESPAWN_TICKS);
		return;
	}

	if (gTP.n != 0 && ped == (void*)gTP.n)
	{
		Hud("You died.", 60);
		jer_npc_health_forget(ped);
		jer_npc_despawn(gTP.n);
		gTP.n = 0;
		gTP.dead = D2PL_RESPAWN_TICKS;
		return;
	}

	for (i = 0; i < D2PL_PENDING_MAX; i++)
	{
		if (gPending[i] == 0 || gPending[i] == ped)
		{
			gPending[i] = ped;
			gPendingTicks[i] = D2PL_RESPAWN_TICKS;
			return;
		}
	}
}

/* damage any ped (player included) and fire the death once; returns health */
int D2plDamage(void* ped, int amount)
{
	int before, after;

	if (ped == 0)
		return 0;

	before = jer_npc_health(ped);

	if (before <= 0)
		return 0;		/* already down: nothing more to take */

	after = jer_npc_damage(ped, amount);

	if (after <= 0)
		D2plOnPedKilled(ped);

	return after;
}

static void PendingTick(void)
{
	int i;

	for (i = 0; i < D2PL_PENDING_MAX; i++)
	{
		if (gPending[i] == 0)
			continue;

		if (--gPendingTicks[i] <= 0)
		{
			jer_npc_set_health(gPending[i], jer_npc_health_max(gPending[i]), 0);
			gPending[i] = 0;
		}
	}
}

/* ------------------------------------------------------------------ */
/* test ped                                                            */
/* ------------------------------------------------------------------ */

int D2plTestPedBehaviour(void)
{
	return gTP.behaviour;
}

const char* D2plTestPedBehaviourName(void)
{
	return gBehaviourNames[gTP.behaviour];
}

void D2plTestPedSetBehaviour(int b)
{
	gTP.behaviour = ((b % D2PL_TP_COUNT) + D2PL_TP_COUNT) % D2PL_TP_COUNT;
	gTP.aim = 0;
	gTP.wanderTicks = 0;
}

int D2plTestPedHealth(void)
{
	return gTP.n != 0 ? jer_npc_health(gTP.n) : -1;
}

void D2plTestPedDespawn(void)
{
	if (gTP.n != 0)
	{
		jer_npc_health_forget(gTP.n);
		jer_npc_despawn(gTP.n);
	}

	gTP.n = 0;
	gTP.dead = 0;
	gTP.tracer = 0;
}

int D2plTestPedSpawn(void)
{
	int p[3];
	int h = gD2plViewHeading;
	int x, z;

	D2plTestPedDespawn();

	if (!PlayerPos(p))
	{
		Hud("TEST PED: get on foot first", 45);
		return 0;
	}

	x = p[0] + D2plSin(h) * D2PL_TP_SPAWN_DIST / 4096;
	z = p[2] + D2plCos(h) * D2PL_TP_SPAWN_DIST / 4096;

	gTP.n = jer_npc_spawn_model(0, x, z);	/* 0 = Tanner */

	if (gTP.n == 0)
	{
		Hud("TEST PED: ped table full", 45);
		return 0;
	}

	jer_npc_set_health(gTP.n, D2PL_PED_HEALTH, D2PL_PED_HEALTH);
	jer_npc_face(gTP.n, (h + 2048 + D2PL_NPC_DIR_OFS) & 0xfff);	/* face the player */
	jer_npc_stop(gTP.n);

	gTP.dead = 0;
	gTP.aim = 0;
	gTP.cooldown = 30;
	gTP.wanderTicks = 0;

	{
		char line[64];

		snprintf(line, sizeof(line), "Test Tanner spawned (%s)", gBehaviourNames[gTP.behaviour]);
		Console(line);
	}

	return 1;
}

static void TestPedShoot(const int from[3], const int target[3], int dist)
{
	int chance = 80 - dist * 50 / D2PL_TP_FIRE_RANGE;	/* 80% point blank .. 30% at range */
	int hit = (rand() % 100) < chance;
	int to[3];

	D2plSfxFire();

	to[0] = target[0];
	to[1] = target[1] - D2PL_TP_CHEST;
	to[2] = target[2];

	if (hit)
	{
		D2plDamage(gD2plPlayerPed0, D2PL_TP_DAMAGE);
		jer_fx_impact(to[0], to[1], to[2], 0, 0, JER_FX_IMPACT_PED);
	}
	else
	{
		/* a miss lands on the ground beside you */
		to[0] += (rand() % 400) - 200;
		to[2] += (rand() % 400) - 200;
		to[1] = target[1] + 120;
		jer_fx_impact(to[0], to[1], to[2], 0, 0, JER_FX_IMPACT_WORLD);
	}

	gTP.tracerFrom[0] = from[0];
	gTP.tracerFrom[1] = from[1] - D2PL_TP_CHEST;
	gTP.tracerFrom[2] = from[2];
	gTP.tracerTo[0] = to[0];
	gTP.tracerTo[1] = to[1];
	gTP.tracerTo[2] = to[2];
	gTP.tracer = 3;
}

static void TestPedPickWander(const int player[3])
{
	int a = rand() & 0xfff;
	int r = D2PL_TP_WANDER_MIN + rand() % (D2PL_TP_WANDER_MAX - D2PL_TP_WANDER_MIN);

	gTP.wander[0] = player[0] + D2plSin(a) * r / 4096;
	gTP.wander[1] = player[2] + D2plCos(a) * r / 4096;
	gTP.wanderTicks = 60 + rand() % 60;
}

static void TestPedTick(void)
{
	int me[3], pl[3], dist, face;

	if (gTP.dead > 0)
	{
		if (--gTP.dead == 0)
			D2plTestPedSpawn();
		return;
	}

	if (gTP.n == 0)
		return;

	if (!jer_npc_owned(gTP.n))
	{
		/* the engine dropped it (level change, pool reuse) */
		gTP.n = 0;
		return;
	}

	if (!jer_npc_get_pos(gTP.n, me) || !PlayerPos(pl))
		return;

	dist = Dist2D(me, pl);
	face = (HeadingTo(me, pl) + D2PL_NPC_DIR_OFS) & 0xfff;

	switch (gTP.behaviour)
	{
		case D2PL_TP_STAND:
			jer_npc_set_gait(gTP.n, face, 0);
			break;

		case D2PL_TP_FOLLOW:
			if (dist > D2PL_TP_FOLLOW_GAP)
				jer_npc_set_gait(gTP.n, face, dist > 1200 ? D2PL_TP_RUN : D2PL_TP_WALK);
			else
				jer_npc_set_gait(gTP.n, face, 0);
			break;

		case D2PL_TP_ATTACK:
		default:
			if (gTP.cooldown > 0)
				gTP.cooldown--;

			if (gTP.aim > 0)
			{
				/* planted, gun on you */
				jer_npc_set_gait(gTP.n, face, 0);

				if (--gTP.aim == 0 && dist <= D2PL_TP_FIRE_RANGE && !gPlayerDead)
				{
					TestPedShoot(me, pl, dist);
					gTP.cooldown = 25 + rand() % 35;
				}
			}
			else if (dist > D2PL_TP_FIRE_RANGE)
			{
				/* too far: pursue */
				jer_npc_set_gait(gTP.n, face, D2PL_TP_RUN);
				gTP.wanderTicks = 0;
			}
			else
			{
				/* in range: wander around the player, stopping to aim */
				int w[3];

				if (gTP.wanderTicks <= 0)
					TestPedPickWander(pl);

				gTP.wanderTicks--;

				w[0] = gTP.wander[0];
				w[1] = me[1];
				w[2] = gTP.wander[1];

				if (Dist2D(me, w) < 120)
					gTP.wanderTicks = 0;

				jer_npc_set_gait(gTP.n, (HeadingTo(me, w) + D2PL_NPC_DIR_OFS) & 0xfff, D2PL_TP_WALK);

				if (gTP.cooldown <= 0 && !gPlayerDead)
					gTP.aim = D2PL_TP_AIM_TICKS;
			}
			break;
	}

	jer_npc_tick(gTP.n);
}

static void TestPedDraw(void)
{
	const JER_HOST_API* api = Api();
	JER_VEC a, b;
	JER_CVEC col;

	if (gTP.tracer <= 0)
		return;

	gTP.tracer--;

	if (api == 0 || api->debug == 0 || api->debug->line_depth == 0)
		return;

	col.r = 255;
	col.g = 200;
	col.b = 60;
	col.code = 0;

	a.x = gTP.tracerFrom[0];
	a.y = -gTP.tracerFrom[1];
	a.z = gTP.tracerFrom[2];
	a.pad = 0;
	b.x = gTP.tracerTo[0];
	b.y = -gTP.tracerTo[1];
	b.z = gTP.tracerTo[2];
	b.pad = 0;

	api->debug->line_depth(&a, &b, &col);
}

static void EnvSpawn(void)
{
	const char* env;

	if (gEnvChecked || gD2plPlayerPed0 == 0)
		return;

	gEnvChecked = 1;
	env = getenv("D2PL_TEST_PED");

	if (env == 0 || env[0] == 0 || env[0] == '0')
		return;

	if (env[0] == 'f' || env[0] == 'F')
		D2plTestPedSetBehaviour(D2PL_TP_FOLLOW);
	else if (env[0] == 'a' || env[0] == 'A')
		D2plTestPedSetBehaviour(D2PL_TP_ATTACK);
	else
		D2plTestPedSetBehaviour(D2PL_TP_STAND);

	D2plTestPedSpawn();
}

/* ------------------------------------------------------------------ */
/* aim target readout                                                  */
/* ------------------------------------------------------------------ */

static void* TraceTarget(void)
{
	int aim[3], d[3], len, steps, i;

	if (!gD2plHandValid || !D2plAimPoint(aim))
		return 0;

	d[0] = aim[0] - gD2plHand[0];
	d[1] = aim[1] - gD2plHand[1];
	d[2] = aim[2] - gD2plHand[2];
	len = (int)sqrtf((float)d[0] * d[0] + (float)d[1] * d[1] + (float)d[2] * d[2]);

	if (len < 1)
		return 0;

	steps = (len > D2PL_TARGET_RANGE ? D2PL_TARGET_RANGE : len) / D2PL_TARGET_RADIUS;

	for (i = 1; i <= steps; i++)
	{
		float t = (float)(i * D2PL_TARGET_RADIUS) / (float)len;
		JerNpc* n = jer_npc_nearest(gD2plHand[0] + (int)(d[0] * t),
			gD2plHand[1] + (int)(d[1] * t), gD2plHand[2] + (int)(d[2] * t), D2PL_TARGET_RADIUS);

		if (n != 0)
			return (void*)n;
	}

	return 0;
}

static void ClearPanels(void)
{
	if (gPanelsOwned)
	{
		jer_hud_panel_clear(D2PL_PANEL_NAME);
		jer_hud_panel_clear(D2PL_PANEL_BAR);
		gPanelsOwned = 0;
	}
}

static void TargetTick(void)
{
	void* t = gD2plAiming ? TraceTarget() : 0;

	if (t != 0)
	{
		gTarget = t;
		gTargetHold = D2PL_TARGET_HOLD;
	}
	else if (gTargetHold > 0)
	{
		gTargetHold--;
	}
	else
	{
		gTarget = 0;
	}

	if (gTarget == 0)
	{
		ClearPanels();
		return;
	}

	{
		char name[32];
		int hp = jer_npc_health(gTarget);
		int max = jer_npc_health_max(gTarget);
		int r, g, b;

		D2plPedName(gTarget, name, sizeof(name));
		jer_hud_panel(D2PL_PANEL_NAME, JER_HUD_ANCHOR_TOP_CENTRE, name, 220, 220, 220);

		D2plHealthColour(hp, max, &r, &g, &b);
		jer_hud_panel_bar(D2PL_PANEL_BAR, JER_HUD_ANCHOR_TOP_CENTRE, hp, max, r, g, b);
		gPanelsOwned = 1;
	}
}

/* ------------------------------------------------------------------ */
/* per tick / per game                                                 */
/* ------------------------------------------------------------------ */

/* JER_EVENT_FRAME, after the weapon update. `onFoot` = the local player is
 * a pedestrian right now. */
void D2plHealthTick(int onFoot)
{
	PendingTick();
	TestPedTick();

	if (!onFoot || gD2plPlayerPed0 == 0)
	{
		jer_hud_player_health(0, 0);
		ClearPanels();
		gTarget = 0;
		return;
	}

	EnvSpawn();

	/* a new movement ped (respawn, level) starts full */
	if (gD2plPlayerPed0 != gPlayerHealthPed)
	{
		gPlayerHealthPed = gD2plPlayerPed0;
		jer_npc_set_health(gD2plPlayerPed0, D2PL_PLAYER_HEALTH, D2PL_PLAYER_HEALTH);
		gPlayerDead = 0;
	}

	if (gPlayerDead > 0 && --gPlayerDead == 0)
	{
		jer_npc_set_health(gD2plPlayerPed0, D2PL_PLAYER_HEALTH, D2PL_PLAYER_HEALTH);
		Console("Player respawned");
	}

	jer_hud_player_health(jer_npc_health(gD2plPlayerPed0), jer_npc_health_max(gD2plPlayerPed0));

	TargetTick();
	TestPedDraw();
}

void D2plHealthReset(void)
{
	int i;

	/* the level tore down every ped: the handles are gone, not ours to free */
	gTP.n = 0;
	gTP.dead = 0;
	gTP.aim = 0;
	gTP.tracer = 0;
	gPlayerDead = 0;
	gPlayerHealthPed = 0;
	gTarget = 0;
	gTargetHold = 0;
	gEnvChecked = 0;

	for (i = 0; i < D2PL_PENDING_MAX; i++)
		gPending[i] = 0;

	ClearPanels();
	jer_npc_health_reset_all();
	jer_hud_player_health(0, 0);
}

/* ------------------------------------------------------------------ */
/* keyboard                                                            */
/* ------------------------------------------------------------------ */

/* the O key, press edge (reload). SDL's state is the window's keyboard. */
int D2plReloadKeyPressed(void)
{
	static int prev = 0;
	const Uint8* keys = SDL_GetKeyboardState(NULL);
	int down = keys != NULL && keys[SDL_SCANCODE_O];
	int edge = down && !prev;

	prev = down;
	return edge;
}

/* ------------------------------------------------------------------ */
/* public names (the 0.12 API surface)                                 */
/* ------------------------------------------------------------------ */

void* gD2plAimedPed = 0;	/* last ped a hit-scan connected with (d2pl_weapon.c) */

int D2plPedHealth(void* ped)
{
	return jer_npc_health(ped);
}

void D2plPedSetHealth(void* ped, int health)
{
	jer_npc_set_health(ped, health, 0);
}

void D2plPedDie(void* ped)
{
	if (ped != 0 && jer_npc_health(ped) > 0)
		D2plDamage(ped, jer_npc_health(ped));
}

int D2plSpawnDebugPed(int behavior)
{
	D2plTestPedSetBehaviour(behavior);
	return D2plTestPedSpawn();
}
