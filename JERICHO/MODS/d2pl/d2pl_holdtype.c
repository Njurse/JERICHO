/* d2pl_holdtype.c — the holdtype registry.
 *
 * A holdtype is the data that says how the upper body carries a weapon: the
 * aim behaviour (right-arm IK at the aim point, torso/neck twist limits) and
 * the idle-hold behaviour (a softer raise while the weapon is out but not
 * aimed). d2pl_pose.c is the only consumer; weapons name one by id.
 * See HOLDTYPES.md for how to add one.
 */

#include "d2pl.h"
#include "jer_anim.h"

#include <string.h>

#define D2PL_UPPER_MASK ((1u << JER_LIMB_LOWERBACK) | (1u << JER_LIMB_JOINT_1) | \
	(1u << JER_LIMB_NECK) | (1u << JER_LIMB_HEAD) | \
	(1u << JER_LIMB_RSHOULDER) | (1u << JER_LIMB_RELBOW) | (1u << JER_LIMB_RHAND))
#define D2PL_LEFT_ARM_MASK ((1u << JER_LIMB_LSHOULDER) | (1u << JER_LIMB_LELBOW) | \
	(1u << JER_LIMB_LHAND))

static D2plHoldtype gHoldtypes[D2PL_MAX_HOLDTYPES];
static int gHoldtypeCount = 0;

int D2plHoldtypeRegister(const D2plHoldtype* h)
{
	if (h == 0 || gHoldtypeCount >= D2PL_MAX_HOLDTYPES)
		return -1;

	gHoldtypes[gHoldtypeCount] = *h;

	return gHoldtypeCount++;
}

const D2plHoldtype* D2plHoldtypeGet(int id)
{
	if (id < 0 || id >= gHoldtypeCount)
		return &gHoldtypes[D2PL_HOLD_NONE];

	return &gHoldtypes[id];
}

int D2plHoldtypeFind(const char* name)
{
	int i;

	for (i = 0; i < gHoldtypeCount; i++)
	{
		if (gHoldtypes[i].name != 0 && strcmp(gHoldtypes[i].name, name) == 0)
			return i;
	}

	return -1;
}

void D2plHoldtypeInit(void)
{
	D2plHoldtype h;

	gHoldtypeCount = 0;

	/* D2PL_HOLD_NONE: bare hands, stock animation untouched */
	memset(&h, 0, sizeof(h));
	h.name = "none";
	D2plHoldtypeRegister(&h);

	/* D2PL_HOLD_PISTOL: one-handed, arm locked on the aim; idle hold keeps
	 * the gun low and forward */
	memset(&h, 0, sizeof(h));
	h.name = "pistol";
	h.mask = D2PL_UPPER_MASK;
	h.aimIk = 1;
	h.idleIk = 1;
	h.idlePitch = 600;		/* ~53 degrees below horizontal */
	h.idleWeight = 1400;	/* /4096: a soft raise, the walk cycle still shows */
	h.torsoLimit = D2PL_TORSO_LIMIT;
	h.headLimit = D2PL_HEAD_LIMIT;
	h.allowBackpedal = 1;
	h.pitchLimit = D2PL_PITCH_LIMIT;
	D2plHoldtypeRegister(&h);

	/* D2PL_HOLD_RIFLE: the machine gun. Two-handed: the left arm reaches for
	 * a fore-grip ahead of the right hand; a stiffer idle, a wider twist */
	memset(&h, 0, sizeof(h));
	h.name = "rifle";
	h.mask = D2PL_UPPER_MASK | D2PL_LEFT_ARM_MASK;
	h.aimIk = 1;
	h.idleIk = 1;
	h.idlePitch = 400;
	h.idleWeight = 2600;
	h.torsoLimit = D2PL_TORSO_LIMIT + 200;
	h.headLimit = D2PL_HEAD_LIMIT;
	h.allowBackpedal = 1;
	h.pitchLimit = D2PL_PITCH_LIMIT;
	h.twoHand = 1;
	h.gripForward = 45;		/* ~a forearm ahead of the trigger hand */
	h.gripElbow[0] = 0;
	h.gripElbow[1] = 0;
	h.gripElbow[2] = 200;	/* ~18 degrees of extra elbow bend */
	D2plHoldtypeRegister(&h);
}

D2plHoldtype* D2plHoldtypeEdit(int id)
{
	if (id < 0 || id >= gHoldtypeCount)
		return 0;

	return &gHoldtypes[id];
}
