/*
 * ft_types.h -- the local car view FULL TWIST uses as an external addon.
 *
 * The module never includes engine headers or imports an engine data global.
 * These are view types: ftCarLoad() fills them from the versioned host API and
 * ftCarFlush() writes the fields the module is allowed to change back through
 * the API's car setters.
 */

#ifndef FT_TYPES_H
#define FT_TYPES_H

#include "jericho.h"
#include "jer_host_api.h"

#include <math.h>

#define MAX_CARS                    32
#define MAX_CAR_RESIDENT_MODELS     16

#define CONTROL_TYPE_NONE           0
#define CONTROL_TYPE_PLAYER         1
#define CONTROL_TYPE_CIV_AI         2
#define CONTROL_TYPE_CAMERACOLLIDER 5
#define CONTROL_TYPE_TANNERCOLLIDER 6
#define CONTROL_TYPE_CUTSCENE       7

/* PSX angle units: 4096 = full circle. Hull footprint/contact math only needs
 * the same scale, not the engine's integer table. */
static __inline int FT_RSIN(int a)
{
	return (int)(sin((double)a * 6.28318530717958647692 / 4096.0) * 4096.0);
}

static __inline int FT_RCOS(int a)
{
	return (int)(cos((double)a * 6.28318530717958647692 / 4096.0) * 4096.0);
}

typedef struct FT_VECTOR
{
	int vx, vy, vz, pad;
} VECTOR;

typedef struct FT_SVECTOR
{
	short vx, vy, vz, pad;
} SVECTOR;

typedef struct FT_MATRIX
{
	int m[3][3];
	int t[3];
} MATRIX;

typedef struct FT_WHEEL
{
	int susCompression;
	int locked;
	int onGrass;
	int surface;
} WHEEL;

typedef struct FT_COSMETICS
{
	SVECTOR cog;
	SVECTOR colBox;
	int mass;
	int twistRateX;
	int twistRateY;
	int twistRateZ;
	int cbYoffset;
	int susCoeff;
	int traction;
	int wheelSize;
} CAR_COSMETICS;

typedef struct FT_CAR
{
	void* token;
	int id;
	int controlType;
	int controlFlags;
	int totalDamage;
	int thrust;
	int wheel_angle;
	int padid;
	int direction;

	struct
	{
		int model;
		int palette;
		CAR_COSMETICS* carCos;
		CAR_COSMETICS cos;
	} ap;

	struct
	{
		MATRIX where;
		WHEEL wheel[4];
		int speed;
		int wheel_speed;
		int direction;
		int acc[3];
		int aacc[3];
		unsigned int mayBeColliding;
	} hd;

	struct
	{
		struct
		{
			int angularVelocity[3];
			int linearVelocity[3];
			int fposition[3];
			int orientation[4];
		} n;
	} st;
} CAR_DATA;

/* Only the BUILDING_BOX fields FULL TWIST's scenery gate reads. The prefix of
 * the engine's struct is VECTOR pos, xsize, zsize, theta, height, model. */
typedef struct FT_BUILDING_BOX
{
	VECTOR pos;
	int xsize;
	int zsize;
	int theta;
	int height;
	int model;
} BUILDING_BOX;

#endif /* FT_TYPES_H */
