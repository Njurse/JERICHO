// select/select.h — the CC select flow: a Twisted-Metal style pre-match menu.
//
// Forced with a launch argument (default -ccmenu, or the CC_MENU env var): it
// takes over the frontend start, asks for an arena (the four cities), then a
// vehicle (every registered profile), then begins a match with that
// combination. Built on the JERICHO SDK menu types (JERICHO/include/jer_menu.h).
//
// This is the first cut of the "take a ride" replacement; later it grows into
// the real tournament flow (bracket, garage, per-character cinematics).

#ifndef CD2_SELECT_H
#define CD2_SELECT_H

#include "driver2.h"

// Register the CMDLINE / FRONTEND / FRAME hooks. Called from the module entry.
void cd2SelectRegister(JERICHO_CONTEXT* ctx);

// Non-zero while a CC select menu is on screen (the frontend is frozen).
int cd2SelectActive(void);

#endif /* CD2_SELECT_H */
