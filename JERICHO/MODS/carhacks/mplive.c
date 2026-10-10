/* carhacks/mplive.c — the bridge to mp's live "Change car" row.
 *
 * mp's Multiplayer pause page has a Change car picker: pick a city, pick one of
 * that city's cars, and have it spawn as a replacement for the vehicle you are
 * driving. The picker itself lives in mp (it is mp's menu), but the two things it
 * cannot know on its own live here, and are asked for over custom JERICHO events
 * (../mp/mp_carquery.h is the one place the ids and structs are written down):
 *
 *   MP_CARQ_CITIES  "which cities can this session offer?"
 *   MP_CARQ_SLOTS   "which slots may a city be offered in, and what models?"
 *   MP_CARQ_LOAD    "make (city, model) available here"
 *   MP_CARQ_CHOSEN  "the switch happened (or did not): this is what we drive now"
 *
 * LOAD and CHOSEN are two events on purpose. The session is told what this machine drives
 * only after the switch SUCCEEDED (CHOSEN) - telling it at load time announced a car that
 * might then fail to build or never be driven, and every other machine built it anyway. And
 * CHOSEN is the moment the OLD car's slot can be offered back (net.c,
 * chkNetReleaseSlotIfUnused), which a load alone cannot know.
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
#include "jer_console.h"	/* SLOT-TRACE headline lines also go to the on-screen console */

#include "cars.h"		/* gCarCleanModelPtr: is the built mesh really there */
#include "mission.h"
#include "players.h"		/* player[0].playerCarId: the local player's car_data entry */
#include "texture.h"		/* CarSlotResReport: the per-switch resource dump */

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

	for (city = 0; city < CITY_COUNT && a->count < a->max; city++)
		a->cities[a->count++] = city;

	return JER_RESULT_CONTINUE;
}

static int ChkMpSlots(void* userdata, void* args)
{
	MP_CARQ_SLOTS_ARGS* a = (MP_CARQ_SLOTS_ARGS*)args;
	int slot;

	(void)userdata;

	if (a == NULL || a->slots == NULL || a->models == NULL || a->max <= 0)
		return JER_RESULT_CONTINUE;

	a->count = 0;

	/* The same hack that owns the frontend list owns this answer, so a session cannot be
	 * offered a wider list than the menus would show. Leaving count at 0 is the answer
	 * "nobody knows": mp then keeps its own list, which without this hack is what the
	 * frontend table says anyway. */
	if (!carhacks_enabled(CHK_HACK_UNLOCK_EXTRA))
		return JER_RESULT_CONTINUE;

	for (slot = 0; slot < CHK_CAR_SLOT_COUNT && a->count < a->max; slot++)
	{
		if (chkImportCanOffer(a->city, slot) != CHK_OFFER_OK)
			continue;

		a->slots[a->count] = slot;
		a->models[a->count] = chkImportSlotModel(a->city, slot);
		a->count++;
	}

	return JER_RESULT_CONTINUE;
}

static int ChkMpLoad(void* userdata, void* args)
{
	MP_CARQ_LOAD_ARGS* a = (MP_CARQ_LOAD_ARGS*)args;
	CHK_CAR_ID id;
	int slot, count = 0, fresh, reused = 0;

	(void)userdata;

	if (a == NULL)
		return JER_RESULT_CONTINUE;

	a->ok = 0;

	if (!carhacks_enabled(CHK_HACK_CROSS_CITY))
		return JER_RESULT_CONTINUE;

	if (a->city < 0 || a->city >= CHK_CAR_CITY_COUNT || a->model < 0 || a->model > 12)
	{
		/* SAY SO. This used to read "a->city > 3" and return in silence, so a
		 * request for a Driver 1 car-data city was declined with no trace
		 * anywhere - mp asked, nothing happened, and the player saw only
		 * "not loaded here". It looked like nothing was ever requested. */
		printInfo("[carhacks/mp] change car: REFUSED %s model %d - city out of range "
			  "(0..%d)\n",
			  chkCityName(a->city), a->model, CHK_CAR_CITY_COUNT - 1);
		return JER_RESULT_CONTINUE;
	}

	id = chkCarId(a->city, a->model);

	/* DIAGNOSTIC (CHK_DIAG_SLOT_TRACE): prove where the new car lands relative to the
	 * slot the local player is vacating. The premise this tests: a local change takes a
	 * FRESH spare (chkImportCanonicalSlot) while the old slot stays `used` until
	 * chkNetLocalSwitched releases it AFTER the adopt -- one change holds two slots. */
	if (getenv("CHK_DIAG_SLOT_TRACE") != NULL)
	{
		CHK_CAR_ID was = chkNetLocalCar();
		int oldSlot = chkCarIdIsSet(was) ? chkImportSlotOfCar(was) : -1;
		int s;

		jer_console_log("[carhacks/mp] change: %s model %d (was slot %d)",
			chkCityName(a->city), a->model, oldSlot);

		for (s = CHK_IMPORT_SPARE_FIRST; s < CHK_IMPORT_MAX_SLOTS; s++)
		{
			CHK_CAR_ID held = chkImportSlotId(s);
			int c = chkCarIdCity(held);
			int cars = chkImportCarsOnSlot(s, NULL);

			jer_console_log("[carhacks/mp]   spare %d: held=%d %s model %d (cars on it %d)",
				s, chkImportSlotHeld(s),
				(c >= 0) ? chkCityName(c) : "-", chkCarIdModel(held), cars);
		}
	}

	/* A car the set already holds keeps the slot it is already in -- re-slotting
	 * it would move a car another player may be driving. Otherwise take the
	 * session's canonical spare for OUR player id: that is the slot every machine
	 * already agrees this player's cars live in. */
	slot = chkImportSlotForCar(a->city, a->model);
	fresh = (slot < 0);		/* this load claims the slot: a failure gives it back */

	if (slot < 0)
	{
		/* RETIRE THE OLD SLOT FIRST. The local player's own change vacates the slot
		 * they are in, so give it straight back BEFORE asking for a spare: the canonical
		 * mapping then sees that slot as free, and a change holds ONE slot instead of two
		 * (the old one kept until chkNetLocalSwitched released it AFTER the adopt). That
		 * two-slot transient is what made three seats changing at once run out of the
		 * four-slot import pool (7..10) with "no spare resident slot".
		 *
		 * Safe because it is synchronous and the slot is the player's own: chkImportReleaseSlot
		 * frees the mesh the player's car is drawn with, but the replacement is built into the
		 * set inside this SAME call (no frame, no draw, runs in between), and mp's adopt then
		 * re-points the car before the next render. The guard insists the sole car on the slot
		 * IS the local player's own car_data entry -- a shared car, a peer's remote copy, or a
		 * leftover civilian all fail it and the ordinary fresh-slot path runs instead. */
		CHK_CAR_ID was = chkNetLocalCar();
		int oldSlot = chkCarIdIsSet(was) ? chkImportSlotOfCar(was) : -1;
		int localCar = player[0].playerCarId;
		int cars, first = -1;
		int retired = 0;

		cars = (oldSlot >= 0) ? chkImportCarsOnSlot(oldSlot, &first) : 0;

		if (oldSlot >= CHK_IMPORT_SPARE_FIRST && oldSlot < CHK_IMPORT_MAX_SLOTS &&
			cars == 1 && first == localCar && localCar >= 0)
		{
			chkImportReleaseSlot(oldSlot);
			retired = 1;
		}

		/* REUSE the slot just vacated: the new car takes the old one's place in the
		 * SAME resident slot, so a change never accumulates a second slot. The canonical
		 * spare stays the fallback for a change that did not retire a slot (a shared car,
		 * a fresh pick with nothing to vacate, an on-foot pick). */
		if (retired && chkImportSlotFree(oldSlot))
		{
			slot = oldSlot;
			reused = 1;
		}
		else
			slot = chkImportCanonicalSlot(id, &count);
	}

	if (slot < 0)
	{
		printInfo("[carhacks/mp] change car: no spare resident slot for %s model %d (%d car(s) wanted)\n",
			chkCityName(a->city), a->model, count);
		return JER_RESULT_CONTINUE;
	}

	if (getenv("CHK_DIAG_SLOT_TRACE") != NULL)
	{
		jer_console_log("[carhacks/mp] -> slot %d (%s)", slot,
			reused ? "REUSED" : "fresh spare");
	}

	chkImportSetSlot(slot, id);
	chkImportHotLoad(slot);

	/* NOT advertised here (L1). The session is told this machine drives the car when mp
	 * reports that the switch happened (MP_CARQ_CHOSEN -> ChkMpChosen below): a load can
	 * still fail, or the switch can still not happen, and an advert sent now had every
	 * other machine build a car nobody was driving. */

	/* It counts as held only when the mesh is really there: mp is about to point a
	 * car at this slot, and a slot with no built geometry is a crash, not a
	 * cosmetic glitch (see mp's MpAdoptRemoteCar). */
	a->ok = (chkImportSlotForCar(a->city, a->model) == slot &&
		JerCarSlotUsable(slot)) ? 1 : 0;

	if (!a->ok)
	{
		printInfo("[carhacks/mp] change car: %s model %d could not be built into slot %d\n",
			chkCityName(a->city), a->model, slot);
		printInfo("[carhacks/mp]   detail: slotOfCar=%d usable=%d resident=%d srcCity=%d\n",
			chkImportSlotForCar(a->city, a->model),
			JerCarSlotUsable(slot),
			residentCarModels[slot],
			GetCarModelSourceCity(slot));

		/* A slot this load claimed and could not fill: offer it straight back (the routine
		 * keeps it if somebody else turns out to name the car). */
		if (fresh)
			chkNetReleaseSlotIfUnused(slot, "load failed");
	}

	return JER_RESULT_CONTINUE;
}

/* mp says the Change car switch is over (MP_CARQ_CHOSEN; also fired when the player gets into
 * another car after being on foot).
 *
 *   changed = 1: the car on the road IS (city, model) now - city -1 for the level's own car.
 *                If that is not what we last told the session, move the identity (advert, our
 *                row, the host's table and set) and offer the old car's slot back.
 *   changed = 0: the switch did not happen. A car the load put in a slot for it is offered
 *                back (kept if anybody names it).
 *
 * Runs with or without the cross-city hack: the identity is the session's either way, and a
 * player switching between two of the level's own cars still has to be told to the others. */
static int ChkMpChosen(void* userdata, void* args)
{
	MP_CARQ_CHOSEN_ARGS* a = (MP_CARQ_CHOSEN_ARGS*)args;
	CHK_CAR_ID was, now;

	(void)userdata;

	if (a == NULL || a->model < 0 || a->model >= CHK_MODEL_LIMIT ||
	    a->city >= CHK_CAR_CITY_COUNT)
		return JER_RESULT_CONTINUE;

	now = chkCarId((a->city < 0) ? CHK_CITY_NATIVE : a->city, a->model);

	if (a->changed)
	{
		was = chkNetLocalCar();

		if (!chkCarIdEqual(was, now))
			chkNetLocalSwitched(was, now);
		else if (!chkImportChosenIsSet())
			chkImportSetChosen(now);	/* same car, but it is a choice now */

		/* the monitor, on every switch: what each slot holds and the pool's state, so a
		 * leak is a number that only goes one way */
		CarSlotResReport();
	}
	else
	{
		int slot = chkImportSlotOfCar(now);

		if (slot >= 0)
			chkNetReleaseSlotIfUnused(slot, "loaded for a change that did not happen");
	}

	return JER_RESULT_CONTINUE;
}

void chkMpLiveRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, MP_CARQ_CITIES, ChkMpCities, NULL, 0);
	ctx->jer_register_hook(ctx, MP_CARQ_SLOTS, ChkMpSlots, NULL, 0);
	ctx->jer_register_hook(ctx, MP_CARQ_LOAD, ChkMpLoad, NULL, 0);
	ctx->jer_register_hook(ctx, MP_CARQ_CHOSEN, ChkMpChosen, NULL, 0);
}
