#ifndef JER_PED_ARCHIVE_H
#define JER_PED_ARCHIVE_H

/* jer_ped_archive - interface-3 pedestrian/JMPPED archive contract.
 *
 * The archive parser and model/page installation stay engine services (they
 * write engine VRAM and model state). pedhacks owns the lifecycle and exposes
 * that service through this contract, so mp and other modules do not include
 * models.h/pedest.h for JMPPED.
 *
 * Entry indices are the archive's entry order. Slot is a model slot within an
 * entry, 0..5 for the six pedestrian parts. All functions are safe to call
 * before a level is loaded; they return 0/NULL when the archive is unavailable.
 */

#include "jericho.h"

#ifdef __cplusplus
extern "C" {
#endif

int jer_ped_archive_baked_available(void);
int jer_ped_archive_probe(int city);
int jer_ped_archive_import_baked(int city);
int jer_ped_archive_import_build(int city);
void jer_ped_archive_reset(void);
int jer_ped_archive_ready(void);
int jer_ped_archive_install_models(void);

int jer_ped_archive_entry_count(void);
const char* jer_ped_archive_entry_name(int entry);
int jer_ped_archive_model_for_entry(int entry, int slot);	/* 1 = model present */
int jer_ped_archive_model_installed(int slot);			/* 1 = Jericho resident slot present */
int jer_ped_archive_skeleton_generation(void);

#ifdef __cplusplus
}
#endif

#endif /* JER_PED_ARCHIVE_H */
