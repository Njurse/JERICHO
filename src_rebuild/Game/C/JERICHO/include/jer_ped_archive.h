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

#define JER_PED_ARCHIVE_API_VERSION 1u

typedef struct JER_PED_ARCHIVE JER_PED_ARCHIVE;	/* opaque singleton handle */

typedef struct JER_PED_ARCHIVE_STATE
{
	unsigned int size;
	unsigned int version;
	int baked_available;
	int ready;
	int entry_count;
	int installed;
} JER_PED_ARCHIVE_STATE;

typedef struct JER_PED_ARCHIVE_ENTRY
{
	unsigned int size;
	unsigned int version;
	int index;
	const char* name;
	int model_count;
	int available;
} JER_PED_ARCHIVE_ENTRY;

typedef struct JER_PED_ARCHIVE_MODEL_STATE
{
	unsigned int size;
	unsigned int version;
	int entry;
	int slot;
	int present;
	int installed;
	int ready;
} JER_PED_ARCHIVE_MODEL_STATE;

typedef struct JER_PED_ARCHIVE_PLAYER_STATE
{
	unsigned int size;
	unsigned int version;
	int player_index;
	int on_foot;
	int entry;
	int ped_type;
} JER_PED_ARCHIVE_PLAYER_STATE;

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
int jer_ped_archive_entry_available(int entry, int city);	/* models + pages ready */
int jer_ped_archive_ensure(int city);				/* import + install for this level */
void jer_ped_archive_forget(void);				/* level restart: drop cached state */
void jer_ped_archive_retry(void);				/* explicit re-selection: clear refusal */
int jer_ped_archive_skeleton_generation(void);

const JER_PED_ARCHIVE* jer_ped_archive_current(void);
int jer_ped_archive_state(const JER_PED_ARCHIVE* archive, JER_PED_ARCHIVE_STATE* out);
int jer_ped_archive_entry_at(const JER_PED_ARCHIVE* archive, int index, JER_PED_ARCHIVE_ENTRY* out);
int jer_ped_archive_model_state(const JER_PED_ARCHIVE* archive, int entry, int slot, int city,
				JER_PED_ARCHIVE_MODEL_STATE* out);
int jer_ped_archive_player_state(const JER_PED_ARCHIVE* archive, JER_PED_ARCHIVE_PLAYER_STATE* out);
int jer_ped_archive_select_palette(const JER_PED_ARCHIVE* archive, int handle);
int jer_ped_archive_install(const JER_PED_ARCHIVE* archive, int entry, int city);

#ifdef __cplusplus
}
#endif

#endif /* JER_PED_ARCHIVE_H */
