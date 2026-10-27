/* pedhacks.c - the JMPPED pedestrian asset module entry.
 *
 * The asset pipeline is compiled in for now. Keeping it separate from mp is
 * the first step in decoupling pedestrian assets from both levels and the
 * multiplayer session; the interface-3 archive contract is added in the
 * following steps.
 */

#include "jericho.h"

#include "pedhacks.h"
#include "jer_ped_archive.h"

#include "driver2.h"
#include "models.h"
#include "pedest.h"
#include "players.h"
#include "main.h"
#include "jer_ped_palette.h"

#include <string.h>

extern int GameLevel;

struct JER_PED_ARCHIVE
{
	int unused;
};

static const JER_PED_ARCHIVE sPedArchiveHandle = { 0 };

int jer_ped_archive_baked_available(void)
{
	return JerPedBakedAvailable();
}

int jer_ped_archive_probe(int city)
{
	return JerPedImportProbe(city);
}

int jer_ped_archive_import_baked(int city)
{
	return JerPedImportBaked(city);
}

int jer_ped_archive_import_build(int city)
{
	return JerPedImportBuild(city);
}

void jer_ped_archive_reset(void)
{
	JerPedImportReset();
}

int jer_ped_archive_ready(void)
{
	return JerPedImportReady();
}

int jer_ped_archive_install_models(void)
{
	return JerPedInstallModels();
}

int jer_ped_archive_entry_count(void)
{
	return JerPedEntryCount();
}

const char* jer_ped_archive_entry_name(int entry)
{
	return JerPedEntryName(entry);
}

int jer_ped_archive_model_for_entry(int entry, int slot)
{
	return JerPedModelForEntry(entry, slot) != NULL;
}

int jer_ped_archive_model_installed(int slot)
{
	if (slot < 0 || slot >= 6)
		return 0;

	return pmJerichoModels[0][slot] != NULL;
}

static int sPedInstalledCity = -1;
static int sPedImportCity = -1;
static int sPedImportFailed;
static int sPedBakedFailed;

int jer_ped_archive_entry_available(int entry, int city)
{
	int i;

	if (entry < 0)
		return 0;

	if (entry == 0)
	{
		for (i = 0; i < 6; i++)
		{
			if (pmJerichoModels[0][i] == NULL)
				return 0;
		}

		/* Native Havana models are already resident; imported models need
		 * their pages/palette installed. */
		return (city == 1) ? 1 : jer_ped_archive_ready();
	}

	if (entry >= jer_ped_archive_entry_count())
		return 0;

	for (i = 0; i < 6; i++)
	{
		if (!jer_ped_archive_model_for_entry(entry, i))
			return 0;
	}

	return jer_ped_archive_ready();
}

static int PedEnsureInstalled(int city)
{
	if (!jer_ped_archive_entry_available(0, city))
		return 0;

	if (sPedInstalledCity == city)
		return 1;

	if (!JerPedInstallModels())
		return 0;

	sPedInstalledCity = city;
	return 1;
}

static int PedEnsureImported(int city)
{
	if (jer_ped_archive_entry_available(0, city))
		return PedEnsureInstalled(city);

	if (sPedImportCity != city)
	{
		sPedImportCity = city;
		sPedImportFailed = 0;
		sPedInstalledCity = -1;
	}

	if (sPedImportFailed)
		return 0;

	if (JerPedBakedAvailable())
	{
		if (JerPedImportBaked(city) && PedEnsureInstalled(city))
			return 1;

		JerPedImportReset();
	}

	if (!JerPedImportProbe(1) || !JerPedImportBuild(1) || !PedEnsureInstalled(city))
	{
		sPedImportFailed = 1;
		JerPedImportReset();
		return 0;
	}

	return jer_ped_archive_entry_available(0, city);
}

int jer_ped_archive_ensure(int city)
{
	if (city < 0)
		return 0;

	if (!sPedBakedFailed && JerPedBakedAvailable() && !jer_ped_archive_ready())
	{
		if (!JerPedImportBaked(city))
		{
			JerPedImportReset();
			sPedBakedFailed = 1;
		}
	}

	return PedEnsureImported(city) && PedEnsureInstalled(city);
}

void jer_ped_archive_forget(void)
{
	sPedInstalledCity = -1;
	sPedImportCity = -1;
	sPedImportFailed = 0;
	sPedBakedFailed = 0;
}

void jer_ped_archive_retry(void)
{
	jer_ped_archive_forget();
}

int jer_ped_archive_skeleton_generation(void)
{
	return JerPedSkeletonGeneration();
}

const JER_PED_ARCHIVE* jer_ped_archive_current(void)
{
	return &sPedArchiveHandle;
}

int jer_ped_archive_state(const JER_PED_ARCHIVE* archive, JER_PED_ARCHIVE_STATE* out)
{
	if (archive != &sPedArchiveHandle || out == NULL)
		return 0;

	memset(out, 0, sizeof(*out));
	out->size = (unsigned int)sizeof(*out);
	out->version = JER_PED_ARCHIVE_API_VERSION;
	out->baked_available = jer_ped_archive_baked_available();
	out->ready = jer_ped_archive_ready();
	out->entry_count = jer_ped_archive_entry_count();
	out->installed = (sPedInstalledCity >= 0);
	return 1;
}

int jer_ped_archive_entry_at(const JER_PED_ARCHIVE* archive, int index, JER_PED_ARCHIVE_ENTRY* out)
{
	if (archive != &sPedArchiveHandle || out == NULL || index < 0 ||
		index >= jer_ped_archive_entry_count())
		return 0;

	memset(out, 0, sizeof(*out));
	out->size = (unsigned int)sizeof(*out);
	out->version = JER_PED_ARCHIVE_API_VERSION;
	out->index = index;
	out->name = jer_ped_archive_entry_name(index);
	out->model_count = 6;
	out->available = jer_ped_archive_entry_available(index, GameLevel);
	return 1;
}

int jer_ped_archive_model_state(const JER_PED_ARCHIVE* archive, int entry, int slot, int city,
				JER_PED_ARCHIVE_MODEL_STATE* out)
{
	if (archive != &sPedArchiveHandle || out == NULL || entry < 0 || slot < 0 || slot >= 6)
		return 0;

	memset(out, 0, sizeof(*out));
	out->size = (unsigned int)sizeof(*out);
	out->version = JER_PED_ARCHIVE_API_VERSION;
	out->entry = entry;
	out->slot = slot;
	out->present = jer_ped_archive_model_for_entry(entry, slot);
	out->installed = (entry == 0) ? jer_ped_archive_model_installed(slot) : 0;
	out->ready = jer_ped_archive_entry_available(entry, city);
	return 1;
}

int jer_ped_archive_player_state(const JER_PED_ARCHIVE* archive, JER_PED_ARCHIVE_PLAYER_STATE* out)
{
	LPPEDESTRIAN pPed;

	if (archive != &sPedArchiveHandle || out == NULL)
		return 0;

	memset(out, 0, sizeof(*out));
	out->size = (unsigned int)sizeof(*out);
	out->version = JER_PED_ARCHIVE_API_VERSION;
	out->player_index = 0;
	out->on_foot = (player[0].playerCarId < 0 && player[0].pPed != NULL);
	pPed = player[0].pPed;
	out->entry = (pPed != NULL) ? pPed->pedEntry : -1;
	out->ped_type = (pPed != NULL) ? pPed->pedType : -1;
	return 1;
}

int jer_ped_archive_select_palette(const JER_PED_ARCHIVE* archive, int handle)
{
	if (archive != &sPedArchiveHandle)
		return 0;

	jer_ped_palette_select(handle);
	return 1;
}

int jer_ped_archive_install(const JER_PED_ARCHIVE* archive, int entry, int city)
{
	if (archive != &sPedArchiveHandle)
		return 0;

	if (entry != 0)
		return 0;

	return jer_ped_archive_ensure(city);
}


JER_MODULE_ENTRY(jer_module_pedhacks_entry)(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_module(ctx,
		"pedhacks",			/* id */
		"Pedestrian Hacks",		/* name */
		"0.1.0",			/* version */
		"JERICHO",			/* author */
		"JMPPED pedestrian asset pipeline: archive lifecycle, character entries, "
		"model/page/palette installation. Decouples pedestrian assets from levels "
		"and multiplayer.",
		"",				/* dependencies */
		JERICHO_SDK_VERSION);		/* SDK this module was built against */

	pedhacks_register(ctx);
}

void pedhacks_register(JERICHO_CONTEXT* ctx)
{
	/* SCAFFOLD: the archive contract lands with the interface-3 pedestrian API.
	 * Keeping the entry re-runnable makes module reloads safe. */
	ctx->jer_log(ctx, "[pedhacks] registered (JMPPED asset module)\n");
}
