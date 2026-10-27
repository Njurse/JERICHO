/* onfoot_boot.c — the -onfoot spawn swap (compiled-in builds only).
 *
 * The engine's -onfoot flag is only a request: it sets gBootOnFoot and waits
 * for a module to perform the swap (the engine cannot spawn the ped before the
 * level's car exists). A runtime DLL cannot read gBootOnFoot or call the ped
 * activation functions, so this file compiles to nothing in an addon build.
 */

#include "jericho.h"

#if !defined(JERICHO_MODULE_BUILD)

/* Engine headers first: this file is the only one that touches game types,
 * and including the SDK host API (which pulls in windows.h) alongside
 * driver2.h clashes on OpenEventW in libapi.h. */
#include "driver2.h"
#include "players.h"
#include "cars.h"
#include "pedest.h"
#include "civ_ai.h"		/* PingOutCar */

extern int gBootOnFoot;

void OnfootBootTick(void)
{
	PLAYER* lp;
	CAR_DATA* cp;

	if (!gBootOnFoot)
		return;

	lp = &MainPlayer;

	if (lp->playerCarId < 0)
	{
		/* already on foot (a mission or another module did it) */
		gBootOnFoot = 0;
		return;
	}

	cp = &car_data[lp->playerCarId];

	/* activate the ped first: ChangeCarPlayerToPed derefs pPed */
	ActivatePlayerPedestrian(cp, NULL, 0, NULL, TANNER_MODEL);

	if (lp->pPed == NULL)
		return;		/* ped table full: retry next frame */

	ChangeCarPlayerToPed(0);
	PingOutCar(cp);		/* remove the spawn car */

	gBootOnFoot = 0;

	jer_log("[onfoot] -onfoot: player switched to the ped\n");
}

#else

void OnfootBootTick(void)
{
}

#endif
