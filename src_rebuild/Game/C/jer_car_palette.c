#include "jer_car_palette.h"

#include "dr2limits.h"	/* MAX_CARS (a plain limits header -- do NOT include dr2types.h here,
						   it needs the full PSX type preamble and breaks the TU) */

typedef struct
{
	int on;
	int r, g, b;
} JER_CAR_COLOUR;

/* One entry per CAR_DATA slot. Inert until a set() turns one on. */
static JER_CAR_COLOUR gCarColour[MAX_CARS];

void jer_car_palette_init(void)
{
	int i;

	for (i = 0; i < MAX_CARS; i++)
	{
		gCarColour[i].on = 0;
		gCarColour[i].r = gCarColour[i].g = gCarColour[i].b = 0;
	}
}

void jer_car_palette_set(int carId, int r, int g, int b)
{
	if (carId < 0 || carId >= MAX_CARS)
		return;

	if (r < 0) r = 0; else if (r > 255) r = 255;
	if (g < 0) g = 0; else if (g > 255) g = 255;
	if (b < 0) b = 0; else if (b > 255) b = 255;

	gCarColour[carId].on = 1;
	gCarColour[carId].r = r;
	gCarColour[carId].g = g;
	gCarColour[carId].b = b;
}

void jer_car_palette_clear(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return;

	gCarColour[carId].on = 0;
}

int jer_car_palette_get(int carId, int* r, int* g, int* b)
{
	if (carId < 0 || carId >= MAX_CARS || !gCarColour[carId].on)
	{
		if (r) *r = -1;
		if (g) *g = -1;
		if (b) *b = -1;
		return 0;
	}

	if (r) *r = gCarColour[carId].r;
	if (g) *g = gCarColour[carId].g;
	if (b) *b = gCarColour[carId].b;
	return 1;
}
