// weapons/special/special.c — shared helpers + registration for the six
// vehicle specials. See special.h.

#include "driver2.h"
#include "cars.h"
#include "jericho.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "weapons/special/special.h"

// The car's forward (m[.][2]) and right (m[.][0]) unit vectors, *4096, y-up.
void cd2SpecFwdRight(const CAR_DATA* cp, VECTOR* fwd, VECTOR* right)
{
	const MATRIX* w = &cp->hd.where;

	fwd->vx = w->m[0][2]; fwd->vy = w->m[1][2]; fwd->vz = w->m[2][2];
	right->vx = w->m[0][0]; right->vy = w->m[1][0]; right->vz = w->m[2][0];
}

// `eighth` eighths of a turn around the car's heading, as a unit *4096 vector
// (0 = forward, 2 = right, 4 = back, 6 = left). The diagonals use 0.7071 *
// 4096 = 2896.
void cd2SpecCompass(const CAR_DATA* cp, int eighth, VECTOR* out)
{
	VECTOR fwd, right;
	int f, r;

	cd2SpecFwdRight(cp, &fwd, &right);

	switch (((eighth % 8) + 8) % 8)
	{
	case 0: f = 4096; r = 0; break;			// forward
	case 1: f = 2896; r = 2896; break;		// forward-right
	case 2: f = 0;    r = 4096; break;		// right
	case 3: f = -2896; r = 2896; break;		// back-right
	case 4: f = -4096; r = 0; break;		// back
	case 5: f = -2896; r = -2896; break;		// back-left
	case 6: f = 0;    r = -4096; break;		// left
	default: f = 2896; r = -2896; break;		// forward-left
	}

	out->vx = (int)(((long long)fwd.vx * f + (long long)right.vx * r) >> 12);
	out->vy = (int)(((long long)fwd.vy * f + (long long)right.vy * r) >> 12);
	out->vz = (int)(((long long)fwd.vz * f + (long long)right.vz * r) >> 12);
}

void cd2SpecialsRegister(JERICHO_CONTEXT* ctx)
{
	cd2SpecialHornetRegister(ctx);
	cd2SpecialAvalancheRegister(ctx);
	cd2SpecialCorvoRegister(ctx);
	cd2SpecialBruxaRegister(ctx);
	cd2SpecialHighwaymanRegister(ctx);
	cd2SpecialDeadstarRegister(ctx);
	cd2SpecialObeliskRegister(ctx);

	ctx->jer_log(ctx, "[cainescrossfire] %d vehicle special(s) registered\n", 7);
}
