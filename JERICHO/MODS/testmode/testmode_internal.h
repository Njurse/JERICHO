#ifndef TESTMODE_INTERNAL_H
#define TESTMODE_INTERNAL_H

/*
 * testmode_internal.h — the module's own state and defaults.
 *
 * Not part of any SDK: only testmode.c should include this.
 */

#include "jericho.h"

/* What the orbit camera is pointed at. */
enum
{
	TESTMODE_SUBJECT_CAR = 0,	/* the player's car for this boot (-car) */
	TESTMODE_SUBJECT_PED = 1	/* a standing Tanner, spawned for the session */
};

/* Where the subject is put: a fixed offset from the player's start position,
 * in engine world units. Sign convention follows the engine (see dr2roads.h). */
#define TESTMODE_DEFAULT_DISTANCE	4200
#define TESTMODE_DEFAULT_HEIGHT		1200
#define TESTMODE_DEFAULT_ORBIT		0		/* degrees, 0 = in front */

#define TESTMODE_MIN_DISTANCE		900
#define TESTMODE_MAX_DISTANCE		12000

typedef struct TESTMODE_CFG
{
	int enabled;			/* is the mode on for this boot */
	int subject;			/* TESTMODE_SUBJECT_* */

	int car;				/* -testcar: model/slot, or -1 for the boot's -car */
	int pedAction;			/* which pose the subject ped is put in */

	int distance;			/* orbit camera: distance from the subject */
	int height;				/* orbit camera: height above it */
	int orbit;				/* orbit camera: yaw around it, degrees */
} TESTMODE_CFG;

/* The one instance, defined in testmode.c. */
extern TESTMODE_CFG gTestCfg;

#endif /* TESTMODE_INTERNAL_H */
