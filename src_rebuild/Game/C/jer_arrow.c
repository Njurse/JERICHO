/* jer_arrow.c - see JERICHO/include/jer_arrow.h.
 *
 * Engine-side, like jer_hud.c: only the game can draw into the OT, and the arrow
 * is the game's own (Draw3DTarget). Modules just fill slots. */

#include "driver2.h"
#include "cars.h"		/* car_data[] */
#include "targets.h"	/* Draw3DTarget, gDraw3DArrowBlue */

#include "JERICHO/include/jer_arrow.h"

#define JER_ARROW_NONE	0
#define JER_ARROW_POINT	1
#define JER_ARROW_CAR	2

typedef struct
{
	int kind;
	int x, y, z;	/* JER_ARROW_POINT, raw game frame */
	int carIndex;	/* JER_ARROW_CAR: index into car_data[] */
} JER_ARROW_SLOT;

static JER_ARROW_SLOT sArrows[JER_ARROW_MAX];

static int ArrowCarIndex(void* car)
{
	CAR_DATA* cp = (CAR_DATA*)car;

	if (cp == NULL || cp < &car_data[0] || cp >= &car_data[MAX_CARS])
		return -1;

	return (int)(cp - &car_data[0]);
}

int jer_arrow_at(int slot, int x, int y, int z)
{
	if (slot < 0 || slot >= JER_ARROW_MAX)
		return 0;

	sArrows[slot].kind = JER_ARROW_POINT;
	sArrows[slot].x = x;
	sArrows[slot].y = y;
	sArrows[slot].z = z;
	sArrows[slot].carIndex = -1;

	return 1;
}

int jer_arrow_on_car(int slot, void* car)
{
	int idx;

	if (slot < 0 || slot >= JER_ARROW_MAX)
		return 0;

	idx = ArrowCarIndex(car);

	if (idx < 0 || car_data[idx].controlType == CONTROL_TYPE_NONE)
	{
		sArrows[slot].kind = JER_ARROW_NONE;
		return 0;
	}

	sArrows[slot].kind = JER_ARROW_CAR;
	sArrows[slot].carIndex = idx;

	return 1;
}

void jer_arrow_clear(int slot)
{
	if (slot < 0 || slot >= JER_ARROW_MAX)
		return;

	sArrows[slot].kind = JER_ARROW_NONE;
}

void jer_arrow_clear_all(void)
{
	int i;

	for (i = 0; i < JER_ARROW_MAX; i++)
		sArrows[i].kind = JER_ARROW_NONE;
}

int jer_arrow_active(int slot)
{
	if (slot < 0 || slot >= JER_ARROW_MAX)
		return 0;

	return sArrows[slot].kind != JER_ARROW_NONE;
}

void jer_arrow_draw(void)
{
	int i, wasBlue;
	VECTOR tv;

	wasBlue = gDraw3DArrowBlue;
	gDraw3DArrowBlue = 0;	/* the stock red arrow */

	for (i = 0; i < JER_ARROW_MAX; i++)
	{
		JER_ARROW_SLOT* a = &sArrows[i];

		if (a->kind == JER_ARROW_POINT)
		{
			tv.vx = a->x;
			tv.vy = -a->y;	/* Draw3DTarget's frame, as DrawWorldTarget feeds it */
			tv.vz = a->z;
		}
		else if (a->kind == JER_ARROW_CAR)
		{
			CAR_DATA* cp = &car_data[a->carIndex];

			/* the car left the world since it was set: the slot is stale */
			if (cp->controlType == CONTROL_TYPE_NONE)
			{
				a->kind = JER_ARROW_NONE;
				continue;
			}

			tv.vx = cp->hd.where.t[0];
			tv.vy = -cp->hd.where.t[1];
			tv.vz = cp->hd.where.t[2];
		}
		else
			continue;

		tv.pad = 0;
		Draw3DTarget(&tv, 0x1);
	}

	gDraw3DArrowBlue = wasBlue;
}
