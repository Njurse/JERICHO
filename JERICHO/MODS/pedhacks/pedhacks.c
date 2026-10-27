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

int jer_ped_archive_skeleton_generation(void)
{
	return JerPedSkeletonGeneration();
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
