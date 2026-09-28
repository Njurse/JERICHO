/* carhacks/carid.h — a car's IDENTITY: (city, model).
 *
 * A model number alone is NOT an identity. Every city ships CARMODEL_0..12, and
 * the same number is a DIFFERENT vehicle in each city (VEHICLES.md) — model 9 is
 * one car in Chicago and another in Rio. So "which car" is a pair:
 *
 *     (city, model)     city 0..3 = CHICAGO/HAVANA/VEGAS/RIO (a LevelNames index)
 *                       model 0..12
 *
 * This is the schema the rest of carhacks is written against, and the one a
 * peer needs to describe a car it is driving: the multiplayer module's wire
 * only carries a model NUMBER (MP_ROSTER_ENTRY.model / MP_CARSTATE_ENTRY.model),
 * which is exactly why a cross-city car is not expressible there today. See
 * MP_ADAPTER.md for the proposed mp-side field and the channel built on this.
 *
 * The wire form is 2 bytes (city, model) and is VERSIONED: a peer that does not
 * understand the version must refuse the set rather than guess.
 *
 * CHK_CITY_NATIVE (0xFF) means "the level's own city" — no import, the stock
 * path. CHK_MODEL_NONE (0xFF) means "no car".
 */
#ifndef CHK_CARID_H
#define CHK_CARID_H

#define CHK_CITY_COUNT_LIMIT	4	/* CHICAGO, HAVANA, VEGAS, RIO */
#define CHK_MODEL_LIMIT		13	/* CARMODEL_0..12 */

#define CHK_CITY_NATIVE		0xFF	/* the level's own city: no import */
#define CHK_MODEL_NONE		0xFF	/* no car */

typedef struct CHK_CAR_ID
{
	unsigned char city;		/* 0..3, or CHK_CITY_NATIVE */
	unsigned char model;		/* 0..12, or CHK_MODEL_NONE */
} CHK_CAR_ID;

/* The wire form. City first, then model, one byte each; nothing else. */
#define CHK_CAR_WIRE_VERSION	1
#define CHK_CAR_WIRE_BYTES	2

/* An unset identity: no city, no car. */
#define CHK_CAR_ID_UNSET	{ CHK_CITY_NATIVE, CHK_MODEL_NONE }

static inline CHK_CAR_ID chkCarId(int city, int model)
{
	CHK_CAR_ID id;

	id.city = (unsigned char)city;
	id.model = (unsigned char)model;

	return id;
}

/* A usable identity: a real model, with a city that is either one of the four or
 * CHK_CITY_NATIVE (the level's own city - no import implied). CHK_MODEL_NONE is
 * what makes an identity "unset", so a native car is a SET identity too: a peer
 * still needs to know which model it is. */
static inline int chkCarIdIsSet(CHK_CAR_ID id)
{
	return (id.model < CHK_MODEL_LIMIT) &&
		(id.city == CHK_CITY_NATIVE || id.city < CHK_CITY_COUNT_LIMIT);
}

/* Set, but with no import implied (the level's own city). */
static inline int chkCarIdIsNative(CHK_CAR_ID id)
{
	return chkCarIdIsSet(id) && (id.city == CHK_CITY_NATIVE);
}

static inline int chkCarIdEqual(CHK_CAR_ID a, CHK_CAR_ID b)
{
	return (a.city == b.city) && (a.model == b.model);
}

static inline int chkCarIdCity(CHK_CAR_ID id)
{
	return (id.city == CHK_CITY_NATIVE) ? -1 : (int)id.city;
}

static inline int chkCarIdModel(CHK_CAR_ID id)
{
	return (id.model == CHK_MODEL_NONE) ? -1 : (int)id.model;
}

static inline void chkCarIdEncode(CHK_CAR_ID id, unsigned char* out)
{
	out[0] = id.city;
	out[1] = id.model;
}

static inline CHK_CAR_ID chkCarIdDecode(const unsigned char* in)
{
	return chkCarId(in[0], in[1]);
}

#endif /* CHK_CARID_H */
