// weapons/core/weapons.c — Combat D2 weapon framework CORE.
//
// Owns the parts every weapon shares:
//   * the registry (CD2_WEAPON_DEF rows, grouped by functional class),
//   * the per-car inventory (ammo + the selected weapon),
//   * the shared world helpers (draw lines, hit tests, damage, muzzle),
//   * the JER_EVENT_FRAME input router + class step loop,
//   * the JER_EVENT_DRAW_WORLD / DRAW_OVERLAY presentation,
//   * the CAMERA_LOOK suppression that frees the side-view keys,
//   * JER_EVENT_GAME_START reset (+ optional "all weapons" test grant).
//
// The class pools live in weapons/raycast, weapons/projectile and
// weapons/drops; the explosion helper in weapons/aoe; the concrete weapon
// defs + fire functions in each class folder next to their pool.
//
// INPUT (TMB-style dual trigger; the stock side-view keys are disabled):
//   LT / L2  - hold to fire the BASE machine gun (auto)
//   RT / R2  - fire the SELECTED weapon (single shot per press)
//   RB / R1  - next weapon     LB / L1 - previous weapon
// The base weapon (machine gun) is never part of the cycle.

#include "driver2.h"
#include "cainescrossfire.h"
#include "cars.h"
#include "cosmetic.h"
#include "camera.h"
#include "players.h"
#include "pad.h"
#include "dr2roads.h"
#include "job_fx.h"
#include "bcollide.h"
#include "sound.h"
#include "gamesnd.h"
#include "jer_sound.h"		/* module sound bank + jer_sound_unlock */
#include "pres.h"
#include "draw.h"
#include "system.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_math.h"
#include "dr2math.h"
#include "weapon.h"
#include "weapon_internal.h"
#include "../../arenas/profile.h"	/* cd2EditorActive - the arena editor owns the pads */
#include "weapons/special/special.h"	/* the six vehicle specials */
#include "crew.h"		/* mounted-crew lean request (fired weapons) */

#include <stdio.h>

// A real entropy source for the weapon RNG (see cd2WpnRand below). We can't
// use <time.h> - the game's include path has its own Game/C/time.h that shadows
// the CRT header. On MSVC x86/x64 the CPU timestamp counter is reliable per
// run; elsewhere the ASLR addresses fall back.
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <intrin.h>
#define CD2_WPN_HAVE_RDTSC 1
#endif

// ---------------------------------------------------------------------------
// Weapon RNG: per-run seeded, and it ADVANCES per call so several draws in the
// SAME frame differ. Random2() is a pure function of the frame counter, so it
// cannot be used for anything spread across one frame (a shotgun's pellets).
// ---------------------------------------------------------------------------
static unsigned int sWpnRng;
static int sWpnRngInit;

int cd2WpnRand(int n)
{
	if (!sWpnRngInit)
	{
		int probe;
		unsigned int s = (unsigned int)(size_t)&sWpnRng;	// ASLR

		s ^= (unsigned int)(size_t)&probe;			// stack
#if defined(CD2_WPN_HAVE_RDTSC)
		s ^= (unsigned int)__rdtsc();
		s ^= (unsigned int)(__rdtsc() >> 32);
#endif
		s = s * 2654435761u + 2246822519u;	// avalanche
		if (s == 0)
			s = 0x9E3779B9u;

		sWpnRng = s;
		sWpnRngInit = 1;
	}

	sWpnRng = sWpnRng * 1664525u + 1013904223u;	// Numerical Recipes LCG

	if (n <= 0)
		return 0;

	return (int)((sWpnRng >> 16) % (unsigned int)n);
}

// ---------------------------------------------------------------------------
// Input bindings (physical buttons are remapped by the engine's config.ini)
// ---------------------------------------------------------------------------
#define CD2_WPN_FIRE_BASE	MPAD_L2	// left trigger: base machine gun
#define CD2_WPN_FIRE_SEL	MPAD_R2	// right trigger: selected weapon
#define CD2_WPN_NEXT		MPAD_R1	// right bumper: next weapon
#define CD2_WPN_PREV		MPAD_L1	// left bumper: previous weapon

// ---------------------------------------------------------------------------
// Sound helpers (the module's compiled bank; CD2_SND_* in cainescrossfire.h)
// ---------------------------------------------------------------------------
// The bank is requested once at registration and appended to SOUND_BANK_SFX at
// every level load. These helpers ask for its CURRENT base each time, so a
// level change cannot leave a stale index behind. A missing bank, or an
// optional slot the builder omitted, makes cd2WpnSndSample return -1 and the
// caller's stock sample is used instead.
static int sWpnSndReq = -1;

#define CD2_WPN_TRAVEL_MAX	4	// concurrent projectile loops (voice budget)
static int sWpnTravelLive;

int cd2WpnSndSample(int id)
{
	int base, count;

	if (id <= CD2_SND_NONE || sWpnSndReq < 0)
		return -1;

	count = jer_sound_mod_bank_count(sWpnSndReq);
	base = jer_sound_mod_bank_base(sWpnSndReq);

	if (base < 0 || id > count)
		return -1;

	return base + id - 1;
}

int cd2WpnSndPlay(int id, int x, int y, int z, int volume, int pitch)
{
	int sample = cd2WpnSndSample(id);

	if (sample < 0)
		return 0;

	if (pitch <= 0)
		pitch = 4096;

	return Start3DSoundVolPitch(-1, SOUND_BANK_SFX, sample, x, y, z,
		volume, pitch) >= 0;
}

int cd2WpnSndPlay2D(int id, int volume, int pitch)
{
	int sample = cd2WpnSndSample(id);

	if (sample < 0)
		return 0;

	if (pitch <= 0)
		pitch = 4096;

	return StartSound(-1, SOUND_BANK_SFX, sample, volume, pitch) >= 0;
}

int cd2WpnSndFire(const CD2_WEAPON_DEF* d, int* channel, int x, int y, int z,
		  int fallbackSample, int fallbackVolume, int fallbackPitch)
{
	int fromBank = (d != NULL) ? cd2WpnSndSample(d->fireSound) : -1;
	int sample = fromBank;
	int pitch;

	if (sample < 0)
	{
		if (fallbackSample < 0)
			return 0;

		sample = fallbackSample;
	}

	pitch = (fromBank >= 0 && d != NULL && d->firePitch > 0)
		? d->firePitch : fallbackPitch;

	if (pitch <= 0)
		pitch = 4096;

	if (channel != NULL && *channel < 0)
		*channel = cd2TakeVoice();

	Start3DSoundVolPitch((channel != NULL) ? *channel : -1,
		SOUND_BANK_SFX, sample, x, y, z, fallbackVolume, pitch);

	return fromBank >= 0;
}

int cd2WpnSndImpact(const CD2_WEAPON_DEF* d, int x, int y, int z,
		    int fallbackSample, int fallbackVolume, int fallbackPitch)
{
	int fromBank = (d != NULL) ? cd2WpnSndSample(d->impactSound) : -1;
	int sample = fromBank;
	int pitch;

	if (sample < 0)
	{
		if (fallbackSample < 0)
			return 0;

		sample = fallbackSample;
	}

	pitch = (fromBank >= 0 && d != NULL && d->impactPitch > 0)
		? d->impactPitch : fallbackPitch;

	if (pitch <= 0)
		pitch = 4096;

	Start3DSoundVolPitch(-1, SOUND_BANK_SFX, sample, x, y, z,
		fallbackVolume, pitch);

	return fromBank >= 0;
}

int cd2WpnSndTravelStart(const CD2_WEAPON_DEF* d, int* channel,
			 const VECTOR* pos, const VECTOR* vel)
{
	int sample;
	int chan;

	if (d == NULL || channel == NULL || pos == NULL)
		return -1;

	sample = cd2WpnSndSample(d->travelSound);

	if (sample < 0 || sWpnTravelLive >= CD2_WPN_TRAVEL_MAX)
		return -1;

	if (*channel < 0)
		*channel = cd2TakeVoice();

	if (*channel < 0)
		return -1;

	chan = Start3DTrackingSound(*channel, SOUND_BANK_SFX, sample,
		(VECTOR*)pos, (LONGVECTOR3*)vel);

	if (chan < 0)
	{
		jer_sound_unlock(*channel);
		*channel = -1;
		return -1;
	}

	if (d->travelPitch > 0)
		SetChannelPitch(chan, d->travelPitch);

	SetChannelVolume(chan, -2200, 0);

	sWpnTravelLive++;
	return chan;
}

void cd2WpnSndTravelStop(int* channel)
{
	if (channel == NULL || *channel < 0)
		return;

	StopChannel(*channel);
	jer_sound_unlock(*channel);
	*channel = -1;

	if (sWpnTravelLive > 0)
		sWpnTravelLive--;
}

// ---------------------------------------------------------------------------
// MG/SMG held-fire loops (see weapon.h)
// ---------------------------------------------------------------------------
static int sMgLoopChan = -1;
static int sMgLoopOn;
static int sSmgLoopChan = -1;
static int sSmgLoopOn;
static int sSmgFrames;

#define CD2_MG_LOOP_VOLUME	-2000
#define CD2_SMG_LOOP_VOLUME	-2600
#define CD2_SMG_LOOP_PITCH	(4096 + 1024)	// "slightly higher" than the MG
#define CD2_SMG_LOOP_TAIL	2

int cd2WpnSndIsLocal(const CAR_DATA* cp)
{
	return (cp != NULL && MainPlayer.playerCarId >= 0 &&
		cp->id == MainPlayer.playerCarId);
}

int cd2WpnSndMgActive(void)
{
	return sMgLoopOn;
}

// Start/stop the player's MG loop from the trigger. Only the local player
// loops; an AI car's MG stays a one-shot (cd2MgFire handles that path).
static void cd2WpnSndMgHold(CAR_DATA* cp, int held)
{
	int sample;

	if (!cd2WpnSndIsLocal(cp))
		return;

	if (!held)
	{
		if (sMgLoopOn && sMgLoopChan >= 0)
			StopChannel(sMgLoopChan);

		sMgLoopOn = 0;
		return;
	}

	if (sMgLoopOn)
		return;

	sample = cd2WpnSndSample(CD2_SND_MG_LOOP);

	if (sample < 0)
		return;

	if (jer_sound_ensure(&sMgLoopChan) < 0)
		return;

	if (Start3DTrackingSound(sMgLoopChan, SOUND_BANK_SFX, sample,
			(VECTOR*)cp->hd.where.t,
			(LONGVECTOR3*)cp->st.n.linearVelocity) < 0)
	{
		jer_sound_unlock(sMgLoopChan);
		sMgLoopChan = -1;
		return;
	}

	SetChannelVolume(sMgLoopChan, CD2_MG_LOOP_VOLUME, 0);
	sMgLoopOn = 1;
}

// One SMG trigger is a six-shot burst; play the same loop for that window at a
// slightly higher pitch. Returns 0 for an AI car or when the bank is absent,
// so the caller keeps its one-shot MG_SHOT path.
int cd2WpnSndSmgBurst(const CAR_DATA* cp)
{
	int sample;

	if (!cd2WpnSndIsLocal(cp))
		return 0;

	sample = cd2WpnSndSample(CD2_SND_MG_LOOP);

	if (sample < 0)
		return 0;

	if (jer_sound_ensure(&sSmgLoopChan) < 0)
		return 0;

	if (Start3DTrackingSound(sSmgLoopChan, SOUND_BANK_SFX, sample,
			(VECTOR*)cp->hd.where.t,
			(LONGVECTOR3*)cp->st.n.linearVelocity) < 0)
	{
		jer_sound_unlock(sSmgLoopChan);
		sSmgLoopChan = -1;
		return 0;
	}

	SetChannelPitch(sSmgLoopChan, CD2_SMG_LOOP_PITCH);
	SetChannelVolume(sSmgLoopChan, CD2_SMG_LOOP_VOLUME, 0);

	sSmgLoopOn = 1;
	sSmgFrames = cd2WdefSmg.burstCount * cd2WdefSmg.burstInterval +
		CD2_SMG_LOOP_TAIL;

	if (sSmgFrames < 1)
		sSmgFrames = 1;

	return 1;
}

// Per-frame router: mgHeld is the player's LT state. The SMG timer is counted
// down here, so the burst loop stops even if no new shot fires.
void cd2WpnSndDrive(void* vcp, int mgHeld)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL)
	{
		cd2WpnSndStopAll();
		return;
	}

	cd2WpnSndMgHold(cp, mgHeld);

	if (sSmgFrames > 0 && --sSmgFrames == 0)
	{
		if (sSmgLoopOn && sSmgLoopChan >= 0)
			StopChannel(sSmgLoopChan);

		sSmgLoopOn = 0;
	}
}

void cd2WpnSndStopAll(void)
{
	if (sMgLoopOn && sMgLoopChan >= 0)
		StopChannel(sMgLoopChan);

	if (sSmgLoopOn && sSmgLoopChan >= 0)
		StopChannel(sSmgLoopChan);

	sMgLoopOn = 0;
	sSmgLoopOn = 0;
	sSmgFrames = 0;
}

// Weapon impact knockback tuning (lever action): spin = leverArm * strength *
// CD2_KNOCK_SCALE, capped at CD2_KNOCK_MAX (world angular-velocity units).
#define CD2_KNOCK_SCALE		6
#define CD2_KNOCK_MAX		0x400000

// ---------------------------------------------------------------------------
// Registry (index == CD2_WID_*)
// ---------------------------------------------------------------------------
static const CD2_WEAPON_DEF* const gWdefs[CD2_WID_COUNT] =
{
	&cd2WdefMG,
	&cd2WdefMissile,
	&cd2WdefMine,
	&cd2WdefHoming,
	&cd2WdefCluster,
	&cd2WdefZoomy,
	&cd2WdefFreeze,
	&cd2WdefShotgun,
	&cd2WdefSmg,
	&cd2WdefSpecialHornet,
	&cd2WdefSpecialAvalanche,
	&cd2WdefSpecialCorvo,
	&cd2WdefSpecialBruxa,
	&cd2WdefSpecialHighwayman,
	&cd2WdefSpecialDeadstar,
	&cd2WdefSpecialObelisk,
	&cd2WdefSpecialBootlegger,
	&cd2WdefSpecialInvocada,
	&cd2WdefSpecialFixer,
	&cd2WdefSpecialWheelman,
	&cd2WdefSpecialDeathcruiser
};

const CD2_WEAPON_DEF* cd2WpnDef(int weaponId)
{
	if (weaponId < 0 || weaponId >= CD2_WID_COUNT)
		return NULL;

	return gWdefs[weaponId];
}

const char* cd2WpnName(int weaponId)
{
	const CD2_WEAPON_DEF* d = cd2WpnDef(weaponId);

	return d ? d->name : "NONE";
}

// Case-insensitive equality for a weapon's code/display name.
static int cd2WpnNameEq(const char* a, const char* b)
{
	while (*a != 0 && *b != 0)
	{
		int ca = *a, cb = *b;

		if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
		if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';

		if (ca != cb)
			return 0;

		a++;
		b++;
	}

	return *a == *b;
}

int cd2WpnFindByName(const char* name)
{
	int i;

	if (name == NULL || name[0] == 0)
		return CD2_WID_NONE;

	for (i = 0; i < CD2_WID_COUNT; i++)
	{
		const CD2_WEAPON_DEF* d = gWdefs[i];

		if (d == NULL)
			continue;

		if (d->name != NULL && cd2WpnNameEq(d->name, name))
			return i;
		if (d->displayName != NULL && cd2WpnNameEq(d->displayName, name))
			return i;
	}

	return CD2_WID_NONE;
}

// The full on-screen name: the def's displayName when set, else the short name.
// A special weapon's custom name lives in displayName, deliberately separate
// from its internal id (special_<car>).
const char* cd2WpnDisplayName(int weaponId)
{
	const CD2_WEAPON_DEF* d = cd2WpnDef(weaponId);

	if (d == NULL)
		return "NONE";

	return (d->displayName != NULL) ? d->displayName : d->name;
}

// ---------------------------------------------------------------------------
// Inventory
// ---------------------------------------------------------------------------
static int gCarAmmo[MAX_CARS][CD2_WID_COUNT];	// rounds per CAR per weapon — a
						// contestant's arsenal (above all its
						// SPECIAL) is its own, not a shared pool
static int gSelected = CD2_WID_NONE;	// the PLAYER's armed weapon (never a base)
static int sGrantAllPending;		// deferred all-weapons test grant (see GAME_START)

// The player's car id, or -1 when there is none (yet).
static int cd2WpnPlayerId(void)
{
	int id = player[0].playerCarId;

	return (id >= 0 && id < MAX_CARS) ? id : -1;
}

// ---- car-keyed inventory (used by a special's fire path and by modules) ----
int cd2WpnCarAmmo(void* vcp, int weaponId)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	const CD2_WEAPON_DEF* d = cd2WpnDef(weaponId);

	if (d == NULL)
		return 0;

	if (d->isBase)
		return -1;			// -1 = infinite (base)

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return 0;

	return gCarAmmo[cp->id][weaponId];
}

int cd2WpnCarOwns(void* vcp, int weaponId)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	const CD2_WEAPON_DEF* d = cd2WpnDef(weaponId);

	if (d == NULL)
		return 0;

	if (d->isBase)
		return 1;

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return 0;

	return gCarAmmo[cp->id][weaponId] > 0;
}

void cd2WpnCarGrant(void* vcp, int weaponId, int ammo)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	const CD2_WEAPON_DEF* d = cd2WpnDef(weaponId);
	int cap;

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	if (d == NULL || d->isBase)
		return;

	cap = (d->maxAmmo > 0) ? d->maxAmmo : 9999;
	gCarAmmo[cp->id][weaponId] = jer_clamp_int(ammo, 0, cap);

	// a pickup auto-equips the first weapon the PLAYER ever holds (TMB)
	if (cp->id == cd2WpnPlayerId() && gCarAmmo[cp->id][weaponId] > 0 && gSelected == CD2_WID_NONE)
		gSelected = weaponId;
}

// Burn a round (the trigger calls this once a shot really goes out).
void cd2WpnCarConsume(void* vcp, int weaponId)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	const CD2_WEAPON_DEF* d = cd2WpnDef(weaponId);

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	if (d == NULL || d->isBase)
		return;

	if (gCarAmmo[cp->id][weaponId] > 0)
		gCarAmmo[cp->id][weaponId]--;
}

// Per-car armed weapon: which weapon each car is currently using, or
// CD2_WID_NONE. Kept for every car so the mounted-crew module can read whose
// window a ped should lean out of (CD2_WEAPON_DEF.leanOut). The player's entry
// tracks gSelected; AI cars record what the AI is engaging with.
static int gCarWeapon[MAX_CARS];

// The weaponId-only accessors mean "the PLAYER's" — the HUD, the pause menu and
// the debug driver all ask about the player's own arsenal.
int cd2WpnOwns(int weaponId)
{
	int id = cd2WpnPlayerId();

	if (id < 0)
	{
		const CD2_WEAPON_DEF* d = cd2WpnDef(weaponId);

		return (d != NULL && d->isBase) ? 1 : 0;
	}

	return cd2WpnCarOwns(&car_data[id], weaponId);
}

int cd2WpnAmmo(int weaponId)
{
	int id = cd2WpnPlayerId();

	if (id < 0)
	{
		const CD2_WEAPON_DEF* d = cd2WpnDef(weaponId);

		return (d != NULL && d->isBase) ? -1 : 0;
	}

	return cd2WpnCarAmmo(&car_data[id], weaponId);
}

int cd2WpnSelected(void)
{
	return gSelected;
}

// The first carried non-base weapon, or CD2_WID_NONE.
static int cd2WpnFirstOwned(void)
{
	int i;

	for (i = 0; i < CD2_WID_COUNT; i++)
	{
		const CD2_WEAPON_DEF* d = gWdefs[i];

		if (d != NULL && !d->isBase && cd2WpnOwns(i))
			return i;
	}

	return CD2_WID_NONE;
}

int cd2WpnCycle(int dir)
{
	int n = CD2_WID_COUNT;
	int idx = (gSelected == CD2_WID_NONE) ? (dir > 0 ? -1 : 0) : gSelected;
	int k;

	for (k = 0; k < n; k++)
	{
		const CD2_WEAPON_DEF* d;

		idx = (idx + dir + n) % n;
		d = gWdefs[idx];

		if (d != NULL && !d->isBase && cd2WpnOwns(idx))
		{
			gSelected = idx;
			return idx;
		}
	}

	gSelected = CD2_WID_NONE;
	return gSelected;
}

void cd2WpnGrant(int weaponId, int ammo)
{
	int id = cd2WpnPlayerId();

	if (id < 0)
		return;

	cd2WpnCarGrant(&car_data[id], weaponId, ammo);
}

void cd2WpnClear(int weaponId)
{
	int id = cd2WpnPlayerId();

	if (weaponId < 0 || weaponId >= CD2_WID_COUNT || id < 0)
		return;

	gCarAmmo[id][weaponId] = 0;

	if (gSelected == weaponId)
		gSelected = cd2WpnFirstOwned();
}

// Debug / test: the whole REGULAR arsenal to its max capacity. Specials are NOT
// part of the grant: a special belongs to ONE car and is handed to that car by
// its own profile (cd2VehOnCarStep), so granting every def here would put every
// OTHER car's special in the player's hands too.
void cd2WpnGrantAllMax(void)
{
	int i;
	int id = cd2WpnPlayerId();

	if (id < 0)
		return;

	for (i = 0; i < CD2_WID_COUNT; i++)
	{
		const CD2_WEAPON_DEF* d = gWdefs[i];

		if (d == NULL || d->isBase || d->isSpecial)
			continue;

		gCarAmmo[id][i] = (d->maxAmmo > 0) ? d->maxAmmo : 9999;
	}

	if (gSelected == CD2_WID_NONE)
		gSelected = cd2WpnFirstOwned();
}

// The weapon a car is currently using (CD2_WID_NONE = unarmed). An accessor
// pair rather than the array itself so the AI and the crew can share it.
int cd2WpnCarArmed(const CAR_DATA* cp)
{
	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return CD2_WID_NONE;

	return gCarWeapon[cp->id];
}

void cd2WpnSetCarArmed(const CAR_DATA* cp, int weaponId)
{
	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	gCarWeapon[cp->id] = weaponId;
}

// Force a car onto a weapon. gCarWeapon records it for every car; for the PLAYER
// it also becomes the armed weapon (gSelected). A profile calls this with its
// special, which is what makes the special the car's STARTING weapon.
void cd2WpnCarSelect(void* vcp, int weaponId)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS)
		return;

	gCarWeapon[cp->id] = weaponId;

	if (cp->id == cd2WpnPlayerId())
	{
		gSelected = weaponId;
		printInfo("[cainescrossfire] player starts on %s (weapon %d)\n", cd2WpnDisplayName(weaponId), weaponId);
	}
}

void cd2WpnResetAll(void)
{
	int i, c;

	for (c = 0; c < MAX_CARS; c++)
		for (i = 0; i < CD2_WID_COUNT; i++)
			gCarAmmo[c][i] = 0;

	gSelected = CD2_WID_NONE;

	for (i = 0; i < MAX_CARS; i++)
		gCarWeapon[i] = CD2_WID_NONE;

	cd2RaycastReset();
	cd2ProjectileReset();
	cd2DropReset();
	cd2WpnSndStopAll();
}

// ---------------------------------------------------------------------------
// Shared world helpers
// ---------------------------------------------------------------------------

#ifndef PSX
extern void Debug_AddLine(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);
extern void Debug_AddLineDepth(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);
#endif

void cd2WpnLine(const VECTOR* a, const VECTOR* b, int r, int g, int bl)
{
#ifndef PSX
	CVECTOR col = { (unsigned char)r, (unsigned char)g, (unsigned char)bl };

	Debug_AddLineDepth(*((VECTOR*)a), *((VECTOR*)b), col);
#else
	(void)a; (void)b; (void)r; (void)g; (void)bl;
#endif
}

void cd2WpnMark(const VECTOR* p, int r, int g, int bl)
{
#ifndef PSX
	CVECTOR col = { (unsigned char)r, (unsigned char)g, (unsigned char)bl };
	int s = 40;
	VECTOR a, b;

	a.vx = p->vx - s; a.vy = p->vy; a.vz = p->vz;
	b.vx = p->vx + s; b.vy = p->vy; b.vz = p->vz;
	Debug_AddLine(a, b, col);

	a.vx = p->vx; a.vy = p->vy - s; a.vz = p->vz;
	b.vx = p->vx; b.vy = p->vy + s; b.vz = p->vz;
	Debug_AddLine(a, b, col);
#else
	(void)p; (void)r; (void)g; (void)bl;
#endif
}

int cd2WpnPointInCar(const CAR_DATA* cp, const VECTOR* p)
{
	const MATRIX* w = &cp->hd.where;
	const SVECTOR* cb = &cp->ap.carCos->colBox;
	int dx = p->vx - w->t[0];
	int dy = p->vy - w->t[1];
	int dz = p->vz - w->t[2];
	int lx = (int)(((long long)dx * w->m[0][0] + (long long)dy * w->m[1][0] + (long long)dz * w->m[2][0]) >> 12);
	int ly = (int)(((long long)dx * w->m[0][1] + (long long)dy * w->m[1][1] + (long long)dz * w->m[2][1]) >> 12);
	int lz = (int)(((long long)dx * w->m[0][2] + (long long)dy * w->m[1][2] + (long long)dz * w->m[2][2]) >> 12);

	(void)ly;

	if (ABS(lx) > cb->vx / 2)
		return 0;

	if (ABS(lz) > cb->vz / 2)
		return 0;

	if (ABS(ly) > (cb->vy / 2) + 40)
		return 0;

	return 1;
}

// opponent test lives with the AI (avoids pulling ai.h into the core)
extern int cd2AiIsOpponent(const void* car);

// ---------------------------------------------------------------------------
// Lock-on target preference (see weapons/core/weapon.h).
//
// The module's own cars - the player and the AI opponents - are the targets a
// lock-on is FOR; stock civ traffic is collateral. The pick loops stay as they
// are (nearest wins); they only score candidates through cd2WpnTargetCost,
// which inflates a traffic candidate's distance so an intended target wins
// unless the traffic is far closer.
// ---------------------------------------------------------------------------
int cd2WpnTargetClass(const CAR_DATA* cp)
{
	if (cp == NULL)
		return 2;

	if (cd2OwnsCar((CAR_DATA*)cp))
		return 0;	// the player or an opponent - the intended targets

	if (cd2IsTraffic((CAR_DATA*)cp))
		return 1;	// stock civ traffic - disfavoured, never forbidden

	return 2;		// an empty / NONE / CUTSCENE slot
}

long long cd2WpnTargetCost(long long d2, const CAR_DATA* cp)
{
	int pct;

	if (cd2WpnTargetClass(cp) != 1)
		return d2;

	pct = gCd2Cfg.lockTrafficPenalty;

	if (pct < 100)
		pct = 100;

	return (d2 * pct) / 100;
}

// Last weapon to damage each car, by car id, for kill attribution (-1 = none).
// An ID rather than a CAR_DATA*: the car_data slots are recycled, so a stored
// pointer can end up aliasing whoever occupies that slot now. The kill path
// takes + clears the entry, so a hit long ago cannot credit a later death.
static int gWpnAttacker[MAX_CARS];

void cd2WpnDamageCar(CAR_DATA* cp, const VECTOR* at, int value, const CAR_DATA* owner)
{
	const MATRIX* w = &cp->hd.where;
	int dx = at->vx - w->t[0];
	int dy = at->vy - w->t[1];
	int dz = at->vz - w->t[2];
	int lx = (int)(((long long)dx * w->m[0][0] + (long long)dy * w->m[1][0] + (long long)dz * w->m[2][0]) >> 12);
	int ly = (int)(((long long)dx * w->m[0][1] + (long long)dy * w->m[1][1] + (long long)dz * w->m[2][1]) >> 12);
	int lz = (int)(((long long)dx * w->m[0][2] + (long long)dy * w->m[1][2] + (long long)dz * w->m[2][2]) >> 12);
	int region = 0;
	int dominant = ABS(lx);
	int dom = lx;

	if (ABS(ly) > dominant) { dominant = ABS(ly); dom = ly; }
	if (ABS(lz) > dominant) { dominant = ABS(lz); dom = lz; }

	(void)dominant;

	// region: 0 front, 1 back, 2 right, 3 left, 4 top, 5 bottom
	if (dom == lz) region = (lz > 0) ? 0 : 1;
	else if (dom == lx) region = (lx > 0) ? 3 : 2;
	else region = (ly > 0) ? 4 : 5;

	// GLOBAL weapon damage (cainescrossfire.h weapon_damage): EVERY weapon hit
	// funnels through here - direct hits, burst/volley splash, the AOE blasts,
	// dropped mines and the damaging specials - so this is the one place the
	// whole arsenal's bite is set. The per-target cuts below (opponent, traffic,
	// armor) stack on top of it.
	value = cd2ScaleDamage(value, gCd2Cfg.weaponDamage);

	// opponents take reduced damage (cainescrossfire.h ai_damage_taken), so one
	// weapon hit doesn't end their run outright
	if (cd2AiIsOpponent(cp))
		value = cd2ScaleDamage(value, gCd2Cfg.aiDamageTaken);
	// traffic takes CD2_TRAFFIC_WPN_TAKEN percent instead (400%: weapons are
	// meant to sweep civilians aside, so this is checked second - an opponent
	// is never traffic, but the ordering keeps their reduction authoritative)
	else if (cd2IsTraffic(cp))
		value = cd2ScaleDamage(value, CD2_TRAFFIC_WPN_TAKEN);

	// remember the last weapon to touch it, so the death can be credited. The
	// owner pointer is validated first: car_data slots are recycled (respawns,
	// MP arenas), so a pointer captured at spawn can end up aliasing a
	// different car - without this, its id read back as whoever occupies the
	// slot now, which credited the player for kills it never made.
	if (cp->id >= 0 && cp->id < MAX_CARS)
	{
		int ownerId = -1;

		if (owner != NULL && owner->id >= 0 && owner->id < MAX_CARS &&
			&car_data[owner->id] == owner)
			ownerId = owner->id;

		gWpnAttacker[cp->id] = ownerId;
	}

	ApplyDamage(cp, (char)region, value, 0);
}

int cd2WpnTakeAttacker(int carId)
{
	int owner;

	if (carId < 0 || carId >= MAX_CARS)
		return -1;

	owner = gWpnAttacker[carId];
	gWpnAttacker[carId] = -1;

	return owner;
}

static int cd2Isqrt(int v)
{
	int r = 0;
	int bit = 1 << 30;

	if (v <= 0)
		return 0;

	while (bit > v)
		bit >>= 2;

	while (bit != 0)
	{
		if (v >= r + bit)
		{
			v -= r + bit;
			r = (r >> 1) + bit;
		}
		else
			r >>= 1;

		bit >>= 2;
	}

	return r;
}

// Weapon impact "lever action": the impact point's offset from the car centre
// (the lever) crossed with the shot direction gives the spin axis, so a shot on
// a corner pitches/yaws the car while a dead-centre hit barely moves it. `dir`
// is the impulse direction (the shot's velocity; any magnitude — normalised
// here). Added straight to the world-space angular velocity, which the engine
// integrates as q += (omega (*) q).
void cd2WpnKnock(CAR_DATA* cp, const VECTOR* at, const VECTOR* dir, int strength)
{
	const MATRIX* w = &cp->hd.where;
	int lx = at->vx - w->t[0];
	int ly = at->vy - w->t[1];
	int lz = at->vz - w->t[2];
	int dmag = cd2Isqrt(dir->vx * dir->vx + dir->vy * dir->vy + dir->vz * dir->vz);
	int* av = cp->st.n.angularVelocity;
	int dnx, dny, dnz, cxi, cyi, czi, mag, rate, i;

	if (dmag < 1 || strength <= 0)
		return;

	// unit*4096 shot direction
	dnx = (int)(((long long)dir->vx * 4096) / dmag);
	dny = (int)(((long long)dir->vy * 4096) / dmag);
	dnz = (int)(((long long)dir->vz * 4096) / dmag);

	// cross(lever, dir) >> 12 ~= lever arm (world units) per axis
	cxi = (int)(((long long)ly * dnz - (long long)lz * dny) >> 12);
	cyi = (int)(((long long)lz * dnx - (long long)lx * dnz) >> 12);
	czi = (int)(((long long)lx * dny - (long long)ly * dnx) >> 12);

	mag = cd2Isqrt(cxi * cxi + cyi * cyi + czi * czi);
	if (mag < 1)
		return;

	// spin (world) = unit axis * (arm * hit strength * scale)
	cxi = (int)(((long long)cxi * 4096) / mag);
	cyi = (int)(((long long)cyi * 4096) / mag);
	czi = (int)(((long long)czi * 4096) / mag);

	rate = mag * strength * CD2_KNOCK_SCALE;
	if (rate > CD2_KNOCK_MAX)
		rate = CD2_KNOCK_MAX;

	av[0] += (int)(((long long)cxi * rate) >> 12);
	av[1] += (int)(((long long)cyi * rate) >> 12);
	av[2] += (int)(((long long)czi * rate) >> 12);

	for (i = 0; i < 3; i++)
		av[i] = jer_clamp_int(av[i], -0x800000, 0x800000);
}

// Opponent-AI helper: any live hostile shot closing on `car`?
int cd2WpnIncomingThreat(const CAR_DATA* car, VECTOR* pos, VECTOR* vel)
{
	if (cd2ProjectileThreat(car, pos, vel))
		return 1;

	if (cd2RaycastThreat(car, pos, vel))
		return 1;

	return 0;
}

int cd2WpnPlayerCar(CAR_DATA** out)
{
	int carId = player[0].playerCarId;

	if (carId < 0 || carId >= MAX_CARS)
		return 0;

	*out = &car_data[carId];

	if ((*out)->controlType != CONTROL_TYPE_PLAYER)
		return 0;

	return 1;
}

void cd2WpnForward(const CAR_DATA* cp, VECTOR* out)
{
	const MATRIX* w = &cp->hd.where;

	out->vx = w->m[0][2];
	out->vy = w->m[1][2];
	out->vz = w->m[2][2];
}

void cd2WpnCarVelocity(const CAR_DATA* cp, VECTOR* out)
{
	// linearVelocity is fixed point (4096 = 1 world unit/frame)
	out->vx = FIXEDH(cp->st.n.linearVelocity[0]);
	out->vy = FIXEDH(cp->st.n.linearVelocity[1]);
	out->vz = FIXEDH(cp->st.n.linearVelocity[2]);
}

void cd2WpnMuzzle(const CAR_DATA* cp, int side, VECTOR* out)
{
	const MATRIX* w = &cp->hd.where;
	const SVECTOR* cb = &cp->ap.carCos->colBox;
	int halfLength = cb->vz;
	int halfWidth = (cb->vx * 102) / 200;	// ~1.02 * vx/2
	int muzzleHeight = cb->vy / 4;

	out->vx = w->t[0] + (int)(((long long)w->m[0][2] * halfLength * 3) >> 14);
	out->vy = w->t[1] + (int)(((long long)w->m[1][2] * halfLength * 3) >> 14);
	out->vz = w->t[2] + (int)(((long long)w->m[2][2] * halfLength * 3) >> 14);

	out->vx += (int)(((long long)w->m[0][0] * halfWidth * side) >> 12);
	out->vy += (int)(((long long)w->m[1][0] * halfWidth * side) >> 12);
	out->vz += (int)(((long long)w->m[2][0] * halfWidth * side) >> 12);

	out->vx += (int)(((long long)w->m[0][1] * muzzleHeight) >> 12);
	out->vy += (int)(((long long)w->m[1][1] * muzzleHeight) >> 12);
	out->vz += (int)(((long long)w->m[2][1] * muzzleHeight) >> 12);
}

// A shot's muzzle when a crew member is leaning out: out to the door plane
// (colBox.vx, the body half-width) and up toward the roof line, a little ahead
// of the cabin centre - so the pellets/missile leave from the window the ped
// hangs out of rather than the car's nose. Same matrix columns as cd2WpnMuzzle
// (0 = right/lateral, 1 = up, 2 = forward).
void cd2WpnWindowMuzzle(const CAR_DATA* cp, int side, VECTOR* out)
{
	const MATRIX* w = &cp->hd.where;
	const SVECTOR* cb = &cp->ap.carCos->colBox;
	int door = cb->vx;			// out to the door plane
	int height = (cb->vy * 65) / 100;	// toward the roof line
	int forward = cb->vz / 5;		// roughly the cabin front

	out->vx = w->t[0] + (int)(((long long)w->m[0][2] * forward) >> 12)
	                 + (int)(((long long)w->m[0][0] * door * side) >> 12)
	                 + (int)(((long long)w->m[0][1] * height) >> 12);
	out->vy = w->t[1] + (int)(((long long)w->m[1][2] * forward) >> 12)
	                 + (int)(((long long)w->m[1][0] * door * side) >> 12)
	                 + (int)(((long long)w->m[1][1] * height) >> 12);
	out->vz = w->t[2] + (int)(((long long)w->m[2][2] * forward) >> 12)
	                 + (int)(((long long)w->m[2][0] * door * side) >> 12)
	                 + (int)(((long long)w->m[2][1] * height) >> 12);
}

int cd2WpnMuzzleSide(const CD2_WEAPON_DEF* def)
{
	if (def == NULL)
		return 0;

	if (def->leanOut & CD2_CREW_DRIVER)
		return -1;	// driver -> left window

	if (def->leanOut & CD2_CREW_GUNNER)
		return 1;	// gunner -> right window

	return 0;
}

void cd2WpnShotMuzzle(const CD2_WEAPON_DEF* def, const CAR_DATA* cp, int side, VECTOR* out)
{
	int ws = cd2WpnMuzzleSide(def);

	// A weapon that leans BOTH crew out (leanOut 3) has no single window, so it
	// fires from whichever window the caller asked for - that is how the
	// scatter's alternating `side` puts pellets out of the driver's window AND
	// the gunner's. Anything else keeps its one window (or the body muzzle).
	if (def != NULL && (def->leanOut & CD2_CREW_DRIVER) && (def->leanOut & CD2_CREW_GUNNER))
	{
		cd2WpnWindowMuzzle(cp, (side < 0) ? -1 : 1, out);
		return;
	}

	if (ws != 0)
		cd2WpnWindowMuzzle(cp, ws, out);
	else
		cd2WpnMuzzle(cp, side, out);
}

// ---------------------------------------------------------------------------
// Refire cooldown: every shot from every shooter passes through here, so a
// primary weapon can enforce a minimum gap between refires no matter who
// pulled the trigger (the player's trigger or an opponent's AI).
// ---------------------------------------------------------------------------
static unsigned int sWpnTick;				// frames since boot
static unsigned int sNextFire[MAX_CARS][CD2_WID_COUNT];	// frame each car may next fire each weapon

int cd2WpnTryFire(void* vcp, int weaponId)
{
	CAR_DATA* cp = (CAR_DATA*)vcp;
	const CD2_WEAPON_DEF* d;
	int gap;

	if (cp == NULL || weaponId < 0 || weaponId >= CD2_WID_COUNT)
		return 0;

	if (cp->id < 0 || cp->id >= MAX_CARS)
		return 0;

	d = cd2WpnDef(weaponId);

	if (d == NULL || d->fire == NULL)
		return 0;

	// still cooling down? (unsigned, wrap-safe)
	if ((int)(sNextFire[cp->id][weaponId] - sWpnTick) > 0)
		return 0;

	d->fire(cp);

	// tell the mounted-crew module which window (if any) this weapon leans a
	// ped out of. Fired by the player AND the AI (both come through here), so
	// every contestant car gets crew. leanOut 0 (the base MG) contributes
	// nothing and can never clear a side another weapon wants.
	cd2CrewNotifyFire(cp, d->leanOut);

	gap = d->refireCooldown;

	if (gap <= 0)
		gap = d->fireInterval;

	if (gap <= 0)
		gap = 1;

	sNextFire[cp->id][weaponId] = sWpnTick + (unsigned int)gap;

	return 1;
}

// ---------------------------------------------------------------------------
// JER_EVENT_FRAME: class step loop + weapon input
// ---------------------------------------------------------------------------

static int cd2WpnOnFrame(void* ud, void* args)
{
	static int prevRT, prevLB, prevRB;

	sWpnTick++;
	static int mgClock;
	static unsigned int dbg;

	CAR_DATA* cp = NULL;
	int pad, lt, rt, rb, lb;

	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled)
	{
		prevRT = prevLB = prevRB = 0;
		cd2WpnSndStopAll();
		return JER_RESULT_CONTINUE;
	}

	// RB (R1) is the weapon-cycle button now, but R1 is ALSO the stock horn:
	// the car loop set horn.on from it this frame, so clear it before the
	// sound pass consumes it (FRAME fires after the car loop, before sound).
	{
		int p;
		for (p = 0; p < 2; p++)
			if (player[p].playerCarId >= 0)
				player[p].horn.on = 0;
	}

	if (!cd2WpnPlayerCar(&cp))
	{
		prevRT = prevLB = prevRB = 0;
		cd2WpnSndStopAll();

		// no player car: nothing to fire, but live shots keep flying
		cd2RaycastStep();
		cd2ProjectileStep();
		cd2DropStep();
		return JER_RESULT_CONTINUE;
	}

	// A dead or ice-frozen driver can't work the guns: a totaled car (a burning
	// wreck / waiting to respawn) and an ice-encased car both swallow the weapon
	// buttons, so neither selection nor firing happens this frame. Live shots
	// already in the air keep flying.
	//
	// The ARENA EDITOR is a third such state: while it is active the shoulders
	// belong to the editor (place / cycle / delete / region corners), so the guns
	// are disarmed - and with nothing armed the crew peds retract back inside.
	// This guard is above the raw pad read, so it also stops the base machine gun
	// (which calls d->fire directly, bypassing cd2WpnTryFire).
	if (cd2CarTotaled(cp) || cd2FreezeActive(cp->id) || cd2EditorActive())
	{
		prevRT = prevLB = prevRB = 0;
		cd2WpnSndStopAll();
		cd2WpnSetCarArmed(cp, CD2_WID_NONE);	// downed driver = not armed

		cd2RaycastStep();
		cd2ProjectileStep();
		cd2DropStep();
		return JER_RESULT_CONTINUE;
	}

	// This car's armed weapon, for anyone who cares whose window a crew ped
	// should lean out of (the player's trigger weapon).
	cd2WpnSetCarArmed(cp, gSelected);

	// the deferred all-weapons test grant, now that the player car exists
	if (sGrantAllPending)
	{
		cd2WpnGrantAllMax();
		sGrantAllPending = 0;
	}

	// Arm the crew from the SELECTED weapon every frame: selecting a leaning
	// weapon brings its ped out and it stays out while that weapon is selected
	// (not only at the instant it fires). A non-leaning selection contributes
	// nothing, so an out side still retracts on its own after its hold lapses.
	{
		const CD2_WEAPON_DEF* armed = cd2WpnDef(gSelected);

		cd2CrewArmed(cp, (armed != NULL) ? armed->leanOut : 0);
	}

	pad = Pads[(unsigned char)*cp->ai.padid].mapped;

	if (gCd2Cfg.debugLog)
	{
		static int probed = 0;

		if (!probed)
		{
			probed = 1;
			printInfo("[cainescrossfire] missile model '%s' resolved=%d scale=%d sound=%d\n",
				gCd2Cfg.missileModel, cd2ProjectileModelValid(),
				gCd2Cfg.missileScale, gCd2Cfg.missileSound);
		}
	}

	lt = (pad & CD2_WPN_FIRE_BASE) ? 1 : 0;
	rt = (pad & CD2_WPN_FIRE_SEL) ? 1 : 0;
	rb = (pad & CD2_WPN_NEXT) ? 1 : 0;
	lb = (pad & CD2_WPN_PREV) ? 1 : 0;

	// The player's MG loop follows LT; the SMG burst timer ticks down here,
	// so the burst loop ends even if no further shot is fired.
	cd2WpnSndDrive(cp, lt);

	if (gCd2Cfg.debugLog && (dbg++ & 31) == 0)
		printInfo("[cainescrossfire] wpn frame: pad=0x%04X LT=%d RT=%d LB=%d RB=%d sel=%d ammo=%d\n",
			pad, lt, rt, lb, rb, gSelected, (gSelected != CD2_WID_NONE) ? cd2WpnAmmo(gSelected) : 0);

	// weapon select: bumpers, edge triggered
	if (rb && !prevRB)
		cd2WpnCycle(+1);
	if (lb && !prevLB)
		cd2WpnCycle(-1);

	// base machine gun: LT held, auto fire
	if (lt)
	{
		const CD2_WEAPON_DEF* d = cd2WpnDef(CD2_WID_MG);

		if (mgClock > 0)
			mgClock--;
		else if (d != NULL && d->fire != NULL)
		{
			d->fire(cp);
			mgClock = (d->fireInterval > 0) ? d->fireInterval : 4;
		}
	}
	else
		mgClock = 0;

	// selected weapon: RT, single shot per press
	if (rt && !prevRT)
	{
		const CD2_WEAPON_DEF* d = cd2WpnDef(gSelected);

		if (d != NULL && cd2WpnCarOwns(cp, gSelected) && cd2WpnCarAmmo(cp, gSelected) > 0)
		{
			// burns a round only when the shot really goes out (refire cooldown)
			if (cd2WpnTryFire(cp, gSelected))
			{
				cd2WpnCarConsume(cp, gSelected);

				if (cd2WpnCarAmmo(cp, gSelected) <= 0)
					cd2WpnClear(gSelected);
			}
		}
		else
		{
			// no primary armed: fall back to the base machine gun so the
			// right trigger is never dead
			const CD2_WEAPON_DEF* mg = cd2WpnDef(CD2_WID_MG);

			if (mg != NULL && mg->fire != NULL)
				mg->fire(cp);
		}
	}

	prevRT = rt;
	prevLB = lb;
	prevRB = rb;

	// advance the live shots AFTER the fire pass so a shot fired this frame
	// registers its first move + impact immediately (not one frame late)
	cd2RaycastStep();
	cd2ProjectileStep();
	cd2DropStep();

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// JER_EVENT_CAMERA_LOOK: free the side-view keys for weapon fire
// ---------------------------------------------------------------------------
static int cd2WpnOnLook(void* ud, void* args)
{
	JER_ARGS_CAMERA_LOOK* a = (JER_ARGS_CAMERA_LOOK*)args;
	PLAYER* lp = (PLAYER*)a->player;
	(void)ud;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	// While driving, the L2/R2 side-view look is replaced by weapon fire, so
	// suppress the stock look branches (leaves the engine's normal
	// settle-back lerp untouched: gripOrbit stays 0).
	if (lp != NULL && lp->playerCarId >= 0)
		a->suppress = 1;

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// JER_EVENT_DRAW_WORLD: projectiles + drops into the real ordering table
// ---------------------------------------------------------------------------
static int cd2WpnOnDrawWorld(void* ud, void* args)
{
	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	cd2RaycastDraw();
	cd2ProjectileDraw();
	cd2DropDraw();

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// JER_EVENT_DRAW_OVERLAY: weapon HUD (bottom-left)
// ---------------------------------------------------------------------------
static int cd2WpnOnOverlay(void* ud, void* args)
{
	CAR_DATA* cp = NULL;
	char text[64];

	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	if (!cd2WpnPlayerCar(&cp))
		return JER_RESULT_CONTINUE;

	SetTextColour(200, 200, 200);
	sprintf(text, "Machine Gun");
	PrintString(text, 20, 210);

	if (gSelected == CD2_WID_NONE)
	{
		SetTextColour(160, 160, 160);
		sprintf(text, "> (no primary weapons)");
		PrintString(text, 20, 222);
	}
	else
	{
		SetTextColour(255, 200, 90);
		sprintf(text, "> %s x%d", cd2WpnDisplayName(gSelected), cd2WpnAmmo(gSelected));
		PrintString(text, 20, 222);
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// JER_EVENT_GAME_START: fresh level = fresh inventory (+ test grant)
// ---------------------------------------------------------------------------
static int cd2WpnOnGameStart(void* ud, void* args)
{
	(void)ud;
	(void)args;

	cd2WpnResetAll();

	// The inventory is per-car, and the player car does not exist yet at
	// GAME_START — so the all-weapons test grant is deferred to the first frame
	// the player car is around (cd2WpnOnFrame).
	sGrantAllPending = gCd2Cfg.allWeapons ? 1 : 0;

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Registration (called once by jer_module_cainescrossfire_entry in cainescrossfire.c)
// ---------------------------------------------------------------------------
void cd2WeaponsRegister(JERICHO_CONTEXT* ctx)
{
	// Ask the engine for the compiled weapon bank at every level load. A
	// missing file is not fatal: every sound helper falls back to the stock
	// sample, and the request slot simply reports base=-1/count=0.
	sWpnSndReq = jer_sound_request_mod_bank("cainescrossfire", "weapons",
		SOUND_BANK_SFX);

	if (sWpnSndReq >= 0)
		ctx->jer_log(ctx, "[cainescrossfire] weapon bank requested: slot %d\n",
			sWpnSndReq);
	else
		ctx->jer_log(ctx, "[cainescrossfire] weapon bank request failed - stock sounds\n");

	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2WpnOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2WpnOnGameStart, NULL, 0);
	// the same reset when the game returns to the frontend menus
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_ENTERED, cd2WpnOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA_LOOK, cd2WpnOnLook, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, cd2WpnOnOverlay, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_WORLD, cd2WpnOnDrawWorld, NULL, 0);

	ctx->jer_log(ctx, "[cainescrossfire] weapons registered (SDK v%d)\n", ctx->sdkVersion);
}
