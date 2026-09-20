// profiles/rows/rows.h — the per-vehicle row files, one extern each.
//
// Every profile is its own .c file in this folder (so a new contestant is a new
// file, never an edit to a shared table). This header is the only place they
// are named together, and profiles/registry.c includes it to build the
// manifest. The build globs MODS/<mod>/**/*.c, so adding a row file only needs
// one more extern here and one more line in the manifest.
//
// To add a vehicle:
//   1. copy a row file here, change the values (keep the id order in
//      profile.h's CD2_VEH_* enum);
//   2. add its extern below;
//   3. add it to gVehRows[] in profiles/registry.c.

#ifndef CD2_PROFILE_ROWS_H
#define CD2_PROFILE_ROWS_H

#include "../profile.h"

extern const CD2_VEH_PROFILE cd2VehRowHornet;		// Chicago, spikes
extern const CD2_VEH_PROFILE cd2VehRowAvalanche;	// Vegas, monster-truck crush
extern const CD2_VEH_PROFILE cd2VehRowCorvo;		// Rio, siren + lightning
extern const CD2_VEH_PROFILE cd2VehRowBruxa;		// Rio, double shotgun
extern const CD2_VEH_PROFILE cd2VehRowHighwayman;	// Havana, breath of fire
extern const CD2_VEH_PROFILE cd2VehRowDeadstar;		// Rio, turbo ram
extern const CD2_VEH_PROFILE cd2VehRowObelisk;		// Rio, missile salvo

#endif /* CD2_PROFILE_ROWS_H */
