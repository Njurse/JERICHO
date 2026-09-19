// weapons/special/special.h — the six VEHICLE SPECIAL weapons.
//
// Each vehicle profile (profiles/) owns one unique special. They live here, one
// file each, all driven through the ordinary weapon framework (a CD2_WEAPON_DEF
// row + a fire fn), so a special is fired by the same cd2WpnTryFire the shared
// weapons use - the trigger, the AI and the debug driver all reach it. What a
// special adds beyond a shared weapon is a STATUS: a per-car timed effect with
// its own hooks (the deployed spike ring, the monster-truck crush, the siren's
// revolving bolt, the flame cone, the turbo dash).
//
// Internal ids are CD2_WID_SPECIAL_<CAR> (special_<car>); the on-screen name is
// the def's displayName, and the recharge and ammo are the def's refireCooldown
// and maxAmmo. A vehicle profile only names the weapon.

#ifndef CD2_SPECIAL_H
#define CD2_SPECIAL_H

#include "driver2.h"
#include "weapons/core/weapon.h"

extern const CD2_WEAPON_DEF cd2WdefSpecialHornet;	// special/hornet.c
extern const CD2_WEAPON_DEF cd2WdefSpecialAvalanche;	// special/avalanche.c
extern const CD2_WEAPON_DEF cd2WdefSpecialCorvo;	// special/corvo.c
extern const CD2_WEAPON_DEF cd2WdefSpecialBruxa;	// special/bruxa.c
extern const CD2_WEAPON_DEF cd2WdefSpecialHighwayman;	// special/highwayman.c
extern const CD2_WEAPON_DEF cd2WdefSpecialDeadstar;	// special/deadstar.c

void cd2SpecialHornetRegister(JERICHO_CONTEXT* ctx);
void cd2SpecialAvalancheRegister(JERICHO_CONTEXT* ctx);
void cd2SpecialCorvoRegister(JERICHO_CONTEXT* ctx);
void cd2SpecialBruxaRegister(JERICHO_CONTEXT* ctx);
void cd2SpecialHighwaymanRegister(JERICHO_CONTEXT* ctx);
void cd2SpecialDeadstarRegister(JERICHO_CONTEXT* ctx);

// Register every special's status hooks (called from the module entry).
void cd2SpecialsRegister(JERICHO_CONTEXT* ctx);

// ---------------------------------------------------------------------------
// Small shared helpers (special.c)
// ---------------------------------------------------------------------------
// The car's forward / right unit vectors (*4096), y-up (the heading frame the
// 8-compass specials fan their shots around).
void cd2SpecFwdRight(const CAR_DATA* cp, VECTOR* fwd, VECTOR* right);

// A y-up direction `i` eighths around the car's heading (i = 0..7, 0 = forward,
// 2 = right, 4 = back, 6 = left), as a unit *4096 vector.
void cd2SpecCompass(const CAR_DATA* cp, int eighth, VECTOR* out);

#endif /* CD2_SPECIAL_H */
