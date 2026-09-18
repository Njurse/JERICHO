/* cd2debug.c — TEMPORARY scripted debug driver.
 *
 *   *** REMOVE WHEN DONE ***  delete this file and the three lines that
 *   mention cd2Debug* in cainescrossfire.c.
 *
 * Headless runs have no player input, so anything the player must do by hand
 * (fire, be killed, pick up weapons) cannot be exercised. This drives those
 * events from a timer instead, so a run can be scripted to hit them:
 *
 *   JERICHO/CONFIG/cc_debug.txt, one step per line, '#' comments:
 *
 *     60:killplayer:enemy     # an opponent kills us  -> "You were killed by X"
 *     150:killplayer:self     # self-inflicted         -> "You died"
 *     240:killnpc:player      # we kill an opponent     -> "You killed X"
 *     330:killnpc:npc         # an opponent kills one   -> "X was killed by Y"
 *     420:killnpc:none        # no attacker             -> "X died"
 *     510:grant
 *
 * Grammar: `frame:action[:arg[:arg2]]`, frames counted from GAME_START (the sim steps
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
 *   team:<faction index>:<rrggbb>   change a team's colour at runtime. This is
 *       the only way to see a mid-round colour change (cd2TeamSet) from a
 *       headless run - the HUD, the banner and the suit palette all follow it on
 *       the next draw.
 *   fire:<weapon>                   the player's car fires one named weapon now
 *       (mg/missile/mine/homing/cluster/zoomy/freeze/shotgun) through the same
 *       cd2WpnTryFire the trigger uses - so a headless run can exercise a
 *       leaning weapon (and the mounted crew it calls out).
 *   crew                            dump the mounted-crew state (who is out)
 *       for the player and the first opponent.
 *   muzzle                          print the player car's centred muzzle next
 *       to the left/right window muzzles (leaning weapons fire from a window).
 *   select:<weapon>                 cycle the player's car until that weapon is
 *       selected (grant first: the cycle skips unowned weapons).
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

#include "cainescrossfire.h"
#include "teams/teams.h"		/* cd2TeamSet - change a team mid-round */
#include "turbo/turbo.h"		/* cd2TurboForce - check the boost headlessly */
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "weapons/core/crew.h"
#include "ai/ai.h"

#define CD2_DBG_MAX		16	// scripted steps
#define CD2_DBG_DAMAGE		3000	// per frame, well above one hit's clamp
#define CD2_DBG_KILL_TIMEOUT	300	// give up on a kill after this many frames (10s)

enum { CD2_DBG_NONE = 0, CD2_DBG_KILLPLAYER, CD2_DBG_KILLNPC, CD2_DBG_GRANT, CD2_DBG_FIRE, CD2_DBG_CREW, CD2_DBG_MUZZLE, CD2_DBG_SELECT, CD2_DBG_TEAM, CD2_DBG_TURBO };
enum { CD2_DBG_ATT_ENEMY = 0, CD2_DBG_ATT_SELF, CD2_DBG_ATT_NONE, CD2_DBG_ATT_PLAYER, CD2_DBG_ATT_NPC };

typedef struct CD2_DBG_STEP
{
	int frame;
	int action;
	int arg;
	int arg2;	// optional second value (-1 = not given); hex, so it can carry a colour
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

// A weapon short name -> CD2_WID_*, advancing *s past the name. CD2_WID_NONE if
// the name is unknown. Shared by the `fire:` and `select:` steps.
static int cd2DbgWeaponId(const char** s)
{
	static const struct { const char* n; int id; } wtab[] =
	{
		{ "mg", CD2_WID_MG }, { "missile", CD2_WID_MISSILE },
		{ "mine", CD2_WID_MINE }, { "homing", CD2_WID_HOMING },
		{ "cluster", CD2_WID_CLUSTER }, { "zoomy", CD2_WID_ZOOMY },
		{ "freeze", CD2_WID_FREEZE }, { "shotgun", CD2_WID_SHOTGUN },
		{ "special_jericho", CD2_WID_SPECIAL_JERICHO },
		{ "special", CD2_WID_SPECIAL_JERICHO },
		{ "smg", CD2_WID_SMG },
	};
	const char* p = *s;
	unsigned int w;

	for (w = 0; w < sizeof(wtab) / sizeof(wtab[0]); w++)
	{
		int k = 0;

		while (wtab[w].n[k] != '\0' && p[k] == wtab[w].n[k])
			k++;

		if (wtab[w].n[k] == '\0')
		{
			*s = p + k;
			return wtab[w].id;
		}
	}

	return CD2_WID_NONE;
}

// Parses `action[:arg]`, leaving *s just past it. Returns CD2_DBG_NONE if the
// action name is not recognised.
static int cd2DbgReadAction(const char** s, int* arg)
{
	const char* p = *s;

	*arg = CD2_DBG_ATT_ENEMY;

	if (cd2DbgMatch(&p, "turbo"))
	{
		/* turbo:<0|1> -- force the player's turbo on or off. The double tap is the
		 * real trigger; this is for checking the meter and the boost headlessly. */
		int on = 0;

		if (*p != ':')
			return CD2_DBG_NONE;

		p++;
		on = (*p >= '0' && *p <= '2') ? (*p - '0') : 0;	/* 2 = a reverse boost */

		*arg = on;
		*s = p;
		return CD2_DBG_TURBO;
	}

	if (cd2DbgMatch(&p, "team"))
	{
		/* team:<faction index> -- the caller's optional second argument is the new
		 * colour, as rrggbb. Changes a team mid-round (cd2TeamSet). */
		int fac = 0;

		if (*p != ':')
			return CD2_DBG_NONE;

		p++;

		if (*p < '0' || *p > '9')
			return CD2_DBG_NONE;

		while (*p >= '0' && *p <= '9')
		{
			fac = fac * 10 + (*p - '0');
			p++;
		}

		*arg = fac;
		*s = p;
		return CD2_DBG_TEAM;
	}

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

	// fire:<weapon> - make the PLAYER's car actually fire a named weapon once
	// (through the same cd2WpnTryFire the trigger uses). Headless runs have no
	// pad, so this is how a run exercises a leaning weapon (and thus the crew).
	if (cd2DbgMatch(&p, "fire"))
	{
		if (*p != ':')
			return CD2_DBG_NONE;

		p++;

		*arg = cd2DbgWeaponId(&p);

		if (*arg == CD2_WID_NONE)
			return CD2_DBG_NONE;

		*s = p;
		return CD2_DBG_FIRE;
	}

	// select:<weapon> - make the PLAYER's car cycle until that weapon is
	// SELECTED (grant first: the cycle skips unowned weapons).
	if (cd2DbgMatch(&p, "select"))
	{
		if (*p != ':')
			return CD2_DBG_NONE;

		p++;

		*arg = cd2DbgWeaponId(&p);

		if (*arg == CD2_WID_NONE)
			return CD2_DBG_NONE;

		*s = p;
		return CD2_DBG_SELECT;
	}

	// crew - dump the current mounted-crew state (who is hanging out) for the
	// player and any opponents. The deterministic "MG did not clear the
	// driver" assertion is `fire:shotgun` then `fire:mg` then `crew`.
	if (cd2DbgMatch(&p, "crew"))
	{
		*s = p;
		return CD2_DBG_CREW;
	}

	// muzzle - print the player car's centred muzzle next to the left/right
	// window muzzles, so a run can show a leaning weapon fires from the door.
	if (cd2DbgMatch(&p, "muzzle"))
	{
		*s = p;
		return CD2_DBG_MUZZLE;
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
	sprintf(path, "%s/CONFIG/cc_debug.txt", jer_root_dir());

	fp = fopen(path, "rb");
	if (fp == NULL)
		return 0;

	while (fgets(line, sizeof(line), fp) != NULL && n < CD2_DBG_MAX)
	{
		const char* s = line;
		int frame = 0, action, arg, arg2;

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

		/* an optional second argument, hex because its only user is a colour
		 * (team:<faction>:<rrggbb>) */
		arg2 = -1;

		if (*s == ':')
		{
			int v = 0, digits = 0;

			s++;

			while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))
			{
				v = v * 16 + ((*s <= '9') ? (*s - '0') : ((*s | 0x20) - 'a' + 10));
				digits++;
				s++;
			}

			if (digits > 0)
				arg2 = v;
		}

		sSteps[n].frame = frame;
		sSteps[n].action = action;
		sSteps[n].arg = arg;
		sSteps[n].arg2 = arg2;
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

	case CD2_DBG_FIRE:
	{
		CAR_DATA* cp = cd2DbgPlayer();

		if (cp != NULL && st->arg >= 0 && st->arg < CD2_WID_COUNT)
		{
			int fired = cd2WpnTryFire(cp, st->arg);

			printInfo("[cd2debug] fire %s from car=%d -> %d\n",
				cd2WpnName(st->arg), cp->id, fired);
		}
		break;
	}

	case CD2_DBG_CREW:
	{
		CAR_DATA* cp = cd2DbgPlayer();
		CAR_DATA* op = cd2DbgFirstOpponent(-1);
		int p[3];

		if (cp != NULL)
		{
			printInfo("[cd2debug] crew frame=%d sel=%s player car=%d driver=%d gunner=%d\n",
				sFrame, cd2WpnName(cd2WpnSelected()),
				cp->id, cd2CrewSideOut(cp, CD2_CREW_DRIVER), cd2CrewSideOut(cp, CD2_CREW_GUNNER));

			if (cd2CrewPedPos(cp, CD2_CREW_DRIVER, p))
				printInfo("[cd2debug] crew driverped pos=(%d,%d,%d) carpos=(%d,%d,%d)\n",
					p[0], p[1], p[2], cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2]);
		}

		if (op != NULL)
			printInfo("[cd2debug] crew opponent car=%d driver=%d gunner=%d\n",
				op->id, cd2CrewSideOut(op, CD2_CREW_DRIVER), cd2CrewSideOut(op, CD2_CREW_GUNNER));

		printInfo("[cd2debug] crew peds spawned=%d\n", cd2CrewPedCount());
		break;
	}

	case CD2_DBG_MUZZLE:
	{
		CAR_DATA* cp = cd2DbgPlayer();

		if (cp != NULL && cp->ap.carCos != NULL)
		{
			VECTOR c, l, r;

			cd2WpnMuzzle(cp, 0, &c);
			cd2WpnWindowMuzzle(cp, -1, &l);		// driver (left)
			cd2WpnWindowMuzzle(cp, 1, &r);		// gunner (right)

			printInfo("[cd2debug] muzzle centre=(%d,%d,%d) left=(%d,%d,%d) right=(%d,%d,%d) car=(%d,%d,%d)\n",
				c.vx, c.vy, c.vz, l.vx, l.vy, l.vz, r.vx, r.vy, r.vz,
				cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2]);
		}
		break;
	}

	case CD2_DBG_SELECT:
	{
		int k;

		for (k = 0; k < CD2_WID_COUNT * 3 && cd2WpnSelected() != st->arg; k++)
			cd2WpnCycle(+1);

		printInfo("[cd2debug] select %s -> now %s\n",
			cd2WpnName(st->arg), cd2WpnName(cd2WpnSelected()));
		break;
	}

	case CD2_DBG_TURBO:
		/* the player's car */
		cd2TurboForce((player[0].playerCarId >= 0) ? player[0].playerCarId : 0, st->arg);
		break;

	case CD2_DBG_TEAM:
		/* change a team's colour mid-round; the second argument is the new
		 * colour as rrggbb, or -1 to leave the colour and just relabel */
		if (st->arg2 >= 0)
		{
			cd2TeamSet(st->arg,
				(st->arg2 >> 16) & 0xff, (st->arg2 >> 8) & 0xff, st->arg2 & 0xff, -1);
		}
		else
		{
			printInfo("[cd2debug] team %d: no colour given (team:<faction>:<rrggbb>)\n", st->arg);
		}
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
