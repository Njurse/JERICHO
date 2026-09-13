/* cd2debug.c — TEMPORARY scripted debug driver.
 *
 *   *** REMOVE WHEN DONE ***  delete this file and the three lines that
 *   mention cd2Debug* in combatd2.c.
 *
 * Headless runs have no player input, so anything the player must do by hand
 * (fire, be killed, pick up weapons) cannot be exercised. This drives those
 * events from a timer instead, so a run can be scripted to hit them:
 *
 *   JERICHO/CONFIG/cd2_debug.txt, one step per line, '#' comments:
 *
 *     60:killplayer:enemy     # an opponent kills us  -> "You were killed by X"
 *     150:killplayer:self     # self-inflicted         -> "You died"
 *     240:killnpc:player      # we kill an opponent     -> "You killed X"
 *     330:killnpc:npc         # an opponent kills one   -> "X was killed by Y"
 *     420:killnpc:none        # no attacker             -> "X died"
 *     510:grant
 *
 * Grammar: `frame:action[:arg]`, frames counted from GAME_START (the sim steps
 * at 30 fps, so 30 = 1s). Inactive unless the file exists, so it costs nothing
 * in normal play. Re-read on every GAME_START, so editing the file and
 * restarting the level is enough.
 *
 *   killplayer[:enemy|:self|:none]  total the local player. `enemy` (default)
 *       makes an opponent the attacker -> "You were killed by <role>";
 *       `self`/`none` leave it unattributed -> "You died".
 *   killnpc[:player|:npc|:none]     total the first opponent. `player`
 *       (default) -> "You killed <role>"; `npc` -> "<victim> was killed by
 *       <killer>"; `none` -> "<victim> died".
 *   grant                           every weapon to max (cd2WpnGrantAllMax).
 *
 * A kill applies damage over several frames rather than in one huge hit,
 * because ApplyDamage clamps a single hit, and it stops as soon as the car is
 * totaled (or after a timeout). Deaths then go through the mod's own respawn
 * timer, so respawns get exercised for free.
 */

#include "driver2.h"
#include "cars.h"
#include "players.h"

#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"

#include "combatd2.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "ai/ai.h"

#define CD2_DBG_MAX		16	// scripted steps
#define CD2_DBG_DAMAGE		3000	// per frame, well above one hit's clamp
#define CD2_DBG_KILL_TIMEOUT	300	// give up on a kill after this many frames (10s)

enum { CD2_DBG_NONE = 0, CD2_DBG_KILLPLAYER, CD2_DBG_KILLNPC, CD2_DBG_GRANT };
enum { CD2_DBG_ATT_ENEMY = 0, CD2_DBG_ATT_SELF, CD2_DBG_ATT_NONE, CD2_DBG_ATT_PLAYER, CD2_DBG_ATT_NPC };

typedef struct CD2_DBG_STEP
{
	int frame;
	int action;
	int arg;
} CD2_DBG_STEP;

static CD2_DBG_STEP sSteps[CD2_DBG_MAX];
static int sCount = -1;		// -1 = not parsed yet
static int sFrame;		// frames since GAME_START
static int sKillVictim = -1;	// pending kill: victim car id (-1 = none)
static int sKillOwner = -1;	// ...its attacker
static int sKillTicks;		// frames the pending kill has been running

// ---------------------------------------------------------------------------
// script parsing
// ---------------------------------------------------------------------------

static int cd2DbgMatch(const char** s, const char* word)
{
	int i = 0;

	while (word[i] != '\0' && (*s)[i] == word[i])
		i++;

	if (word[i] != '\0')
		return 0;

	*s += i;
	return 1;
}

// Parses `action[:arg]`, leaving *s just past it. Returns CD2_DBG_NONE if the
// action name is not recognised.
static int cd2DbgReadAction(const char** s, int* arg)
{
	const char* p = *s;

	*arg = CD2_DBG_ATT_ENEMY;

	if (cd2DbgMatch(&p, "killplayer"))
	{
		if (*p == ':')
		{
			p++;

			if (cd2DbgMatch(&p, "self"))
				*arg = CD2_DBG_ATT_SELF;
			else if (cd2DbgMatch(&p, "none"))
				*arg = CD2_DBG_ATT_NONE;
			else if (cd2DbgMatch(&p, "enemy"))
				*arg = CD2_DBG_ATT_ENEMY;
			else
				return CD2_DBG_NONE;
		}

		*s = p;
		return CD2_DBG_KILLPLAYER;
	}

	if (cd2DbgMatch(&p, "killnpc"))
	{
		if (*p == ':')
		{
			p++;

			if (cd2DbgMatch(&p, "player"))
				*arg = CD2_DBG_ATT_PLAYER;
			else if (cd2DbgMatch(&p, "npc"))
				*arg = CD2_DBG_ATT_NPC;
			else if (cd2DbgMatch(&p, "none"))
				*arg = CD2_DBG_ATT_NONE;
			else
				return CD2_DBG_NONE;
		}

		*s = p;
		return CD2_DBG_KILLNPC;
	}

	if (cd2DbgMatch(&p, "grant"))
	{
		*s = p;
		return CD2_DBG_GRANT;
	}

	return CD2_DBG_NONE;
}

static int cd2DbgParse(void)
{
	char path[512];
	FILE* fp;
	char line[192];
	int n = 0;

	// A file rather than a config key: the config store caps a value (a long
	// debug_script came back truncated to ~63 chars, i.e. three steps), and one
	// step per line is easier to edit anyway.
	sprintf(path, "%s/CONFIG/cd2_debug.txt", jer_root_dir());

	fp = fopen(path, "rb");
	if (fp == NULL)
		return 0;

	while (fgets(line, sizeof(line), fp) != NULL && n < CD2_DBG_MAX)
	{
		const char* s = line;
		int frame = 0, action, arg;

		while (*s == ' ' || *s == '\t')
			s++;

		if (*s == '#' || *s == '\r' || *s == '\n' || *s == '\0')
			continue;	// blank or comment

		if (*s < '0' || *s > '9')	// a step must start with its frame
			continue;

		while (*s >= '0' && *s <= '9')
		{
			frame = frame * 10 + (*s - '0');
			s++;
		}

		if (*s != ':')
			continue;

		s++;

		action = cd2DbgReadAction(&s, &arg);
		if (action == CD2_DBG_NONE)
		{
			printInfo("[cd2debug] unknown step: %s", line);
			continue;
		}

		sSteps[n].frame = frame;
		sSteps[n].action = action;
		sSteps[n].arg = arg;
		n++;
	}

	fclose(fp);

	if (n > 0)
		printInfo("[cd2debug] %d scripted step(s) from %s\n", n, path);
	else
		printInfo("[cd2debug] %s has no usable steps\n", path);

	return n;
}

// ---------------------------------------------------------------------------
// car lookup
// ---------------------------------------------------------------------------

static CAR_DATA* cd2DbgPlayer(void)
{
	int id = MainPlayer.playerCarId;

	if (id < 0 || id >= MAX_CARS)
		return NULL;

	return &car_data[id];
}

// First car the module drives (an opponent), skipping a car id.
static CAR_DATA* cd2DbgFirstOpponent(int skipId)
{
	int i;

	for (i = 0; i < MAX_CARS; i++)
	{
		if (i == skipId || car_data[i].controlType == CONTROL_TYPE_NONE)
			continue;

		if (cd2AiIsOpponent(&car_data[i]))
			return &car_data[i];
	}

	return NULL;
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------

// Resolve a kill's attacker for `victim`: what the script asked for, falling
// back to unattributed when the requested car does not exist.
static int cd2DbgAttacker(int arg, CAR_DATA* victim)
{
	int id = (victim != NULL) ? victim->id : -1;

	switch (arg)
	{
	case CD2_DBG_ATT_SELF:
		return id;

	case CD2_DBG_ATT_NONE:
		return -1;

	case CD2_DBG_ATT_PLAYER:
	{
		CAR_DATA* cp = cd2DbgPlayer();
		return (cp != NULL) ? cp->id : -1;
	}

	case CD2_DBG_ATT_NPC:
	{
		CAR_DATA* cp = cd2DbgFirstOpponent(id);
		return (cp != NULL) ? cp->id : -1;
	}

	default:	// CD2_DBG_ATT_ENEMY: an opponent for the player, the player
			// for an opponent - i.e. "someone else did it"
	{
		CAR_DATA* cp = cd2DbgFirstOpponent(id);
		int playerId = (cd2DbgPlayer() != NULL) ? MainPlayer.playerCarId : -1;

		if (id == playerId)
			return (cp != NULL) ? cp->id : -1;

		return playerId;
	}
	}
}

static void cd2DbgStartKill(CAR_DATA* victim, int arg)
{
	if (victim == NULL)
	{
		printInfo("[cd2debug] kill skipped: no such car\n");
		return;
	}

	sKillVictim = victim->id;
	sKillOwner = cd2DbgAttacker(arg, victim);
	sKillTicks = 0;

	printInfo("[cd2debug] killing car=%d owner=%d\n", sKillVictim, sKillOwner);
}

// Applied on every frame: damage accumulates until the car is totaled, which
// keeps the attribution on the most recent hit rather than one ancient one.
static void cd2DbgStepKill(void)
{
	CAR_DATA* victim;
	const CAR_DATA* owner;
	VECTOR at;

	if (sKillVictim < 0)
		return;

	victim = &car_data[sKillVictim];
	owner = (sKillOwner >= 0 && sKillOwner < MAX_CARS) ? &car_data[sKillOwner] : NULL;

	if (cd2CarTotaled(victim) || sKillTicks++ > CD2_DBG_KILL_TIMEOUT)
	{
		if (sKillTicks > CD2_DBG_KILL_TIMEOUT)
			printInfo("[cd2debug] kill car=%d timed out\n", sKillVictim);

		sKillVictim = -1;
		sKillOwner = -1;
		return;
	}

	at.vx = victim->hd.where.t[0];
	at.vy = victim->hd.where.t[1];
	at.vz = victim->hd.where.t[2];

	cd2WpnDamageCar(victim, &at, CD2_DBG_DAMAGE, owner);
}

static void cd2DbgRunStep(const CD2_DBG_STEP* st)
{
	switch (st->action)
	{
	case CD2_DBG_KILLPLAYER:
		cd2DbgStartKill(cd2DbgPlayer(), st->arg);
		break;

	case CD2_DBG_KILLNPC:
		cd2DbgStartKill(cd2DbgFirstOpponent(-1), st->arg);
		break;

	case CD2_DBG_GRANT:
		cd2WpnGrantAllMax();
		printInfo("[cd2debug] granted all weapons\n");
		break;
	}
}

// ---------------------------------------------------------------------------
// hooks
// ---------------------------------------------------------------------------

static int cd2DbgOnGameStart(void* ud, void* args)
{
	(void)ud;
	(void)args;

	sFrame = 0;
	sKillVictim = -1;
	sKillOwner = -1;
	sKillTicks = 0;

	sCount = cd2DbgParse();	// re-read so the file can be edited between levels

	return JER_RESULT_CONTINUE;
}

static int cd2DbgOnFrame(void* ud, void* args)
{
	int i;

	(void)ud;
	(void)args;

	cd2DbgStepKill();

	if (sCount <= 0)
		return JER_RESULT_CONTINUE;

	sFrame++;

	for (i = 0; i < sCount; i++)
	{
		if (sSteps[i].frame == sFrame)
			cd2DbgRunStep(&sSteps[i]);
	}

	return JER_RESULT_CONTINUE;
}

void cd2DebugRegister(JERICHO_CONTEXT* ctx)
{
	sCount = cd2DbgParse();		// 0 when there is no script file

	if (sCount <= 0)
		return;			// inactive without a script

	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2DbgOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2DbgOnFrame, NULL, 0);

	ctx->jer_log(ctx, "[cd2debug] scripted debug driver active\n");
}
