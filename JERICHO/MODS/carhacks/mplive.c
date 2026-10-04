/* carhacks/mplive.c — the bridge to mp's live "Change car" row.
 *
 * mp's Multiplayer pause page has a Change car picker: pick a city, pick one of
 * that city's cars, and have it spawn as a replacement for the vehicle you are
 * driving. The picker itself lives in mp (it is mp's menu), but the two things it
 * cannot know on its own live here, and are asked for over custom JERICHO events
 * (../mp/mp_carquery.h is the one place the ids and structs are written down):
 *
 *   MP_CARQ_CITIES  "which cities can this session offer?"
 *   MP_CARQ_LOAD    "make (city, model) available here and in the session"
 *
 * THE LOAD HALF IS NOT NEW MACHINERY. It is the same sequence a mid-match peer
 * pick already goes through -- put the car in the session's canonical spare slot,
 * then chkImportHotLoad, which reads that city's data in, builds the slot's
 * geometry in the engine's own pool, applies its cosmetics and records its
 * texture pages. What this adds is the OPENING: mp asking for a car the player
 * chose, instead of one that arrived on the wire.
 *
 * Nothing here runs without a live mp session (mp only asks in one), and nothing
 * runs when the cross-city hack is off: then this machine has only the level's own
 * city and mp's picker offers exactly that.
 */
#include "driver2.h"

#include "jericho.h"
#include "jer_net.h"

#include "cars.h"		/* gCarCleanModelPtr: is the built mesh really there */
#include "mission.h"

#include "carid.h"
#include "carimport.h"
#include "carhacks.h"
#include "net.h"
#include "mplive.h"

#include "../mp/mp_carquery.h"

static int ChkMpCities(void* userdata, void* args)
{
	MP_CARQ_CITIES_ARGS* a = (MP_CARQ_CITIES_ARGS*)args;
	int city;

	(void)userdata;

	if (a == NULL || a->cities == NULL || a->max <= 0)
		return JER_RESULT_CONTINUE;

	a->count = 0;

	/* Cross-city off: no extra cities to offer. Leaving count at 0 is the answer
	 * "nobody knows better", and mp then offers the session's own city alone --
	 * exactly right, because without the import this machine only ever has it. */
	if (!carhacks_enabled(CHK_HACK_CROSS_CITY))
		return JER_RESULT_CONTINUE;

	for (city = 0; city < 4 && a->count < a->max; city++)
		a->cities[a->count++] = city;

	return JER_RESULT_CONTINUE;
}

static int ChkMpLoad(void* userdata, void* args)
{
	MP_CARQ_LOAD_ARGS* a = (MP_CARQ_LOAD_ARGS*)args;
	CHK_CAR_ID id;
	int slot, count = 0;

	(void)userdata;

	if (a == NULL)
		return JER_RESULT_CONTINUE;

	a->ok = 0;

	if (!carhacks_enabled(CHK_HACK_CROSS_CITY))
		return JER_RESULT_CONTINUE;

	if (a->city < 0 || a->city > 3 || a->model < 0 || a->model > 12)
		return JER_RESULT_CONTINUE;

	id = chkCarId(a->city, a->model);

	/* A car the set already holds keeps the slot it is already in -- re-slotting
	 * it would move a car another player may be driving. Otherwise take the
	 * session's canonical spare for OUR player id: that is the slot every machine
	 * already agrees this player's cars live in. */
	slot = chkImportSlotForCar(a->city, a->model);

	if (slot < 0)
		slot = chkImportCanonicalSlot(id, &count);

	if (slot < 0)
	{
		printInfo("[carhacks/mp] change car: no spare resident slot for %s model %d (%d car(s) wanted)\n",
			chkCityName(a->city), a->model, count);
		return JER_RESULT_CONTINUE;
	}

	chkImportSetSlot(slot, id);
	chkImportHotLoad(slot);

	/* Tell the session this machine now drives it. The HOST folds every peer's
	 * claim into the agreed set and republishes, which is what makes the OTHER
	 * machines read the city in and build the car too; a client's claim is what
	 * carries it there. */
	chkNetAdvertisePick(a->city, a->model);

	if (jer_net_is_host())
		chkNetPublishSet();

	/* It counts as held only when the mesh is really there: mp is about to point a
	 * car at this slot, and a slot with no built geometry is a crash, not a
	 * cosmetic glitch (see mp's MpAdoptRemoteCar). */
	a->ok = (chkImportSlotForCar(a->city, a->model) == slot &&
		gCarCleanModelPtr[slot] != NULL) ? 1 : 0;

	if (!a->ok)
		printInfo("[carhacks/mp] change car: %s model %d could not be built into slot %d\n",
			chkCityName(a->city), a->model, slot);

	return JER_RESULT_CONTINUE;
}

void chkMpLiveRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, MP_CARQ_CITIES, ChkMpCities, NULL, 0);
	ctx->jer_register_hook(ctx, MP_CARQ_LOAD, ChkMpLoad, NULL, 0);
}
