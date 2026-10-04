#ifndef CHK_MPLIVE_H
#define CHK_MPLIVE_H

/* carhacks/mplive.h — the bridge to mp's live "Change car" row.
 * See ../mp/mp_carquery.h for the event contract, and mplive.c for what the
 * load half actually does. */

#include "jericho.h"

/* Register the handlers for mp's live-car questions. Called from
 * carhacks_register; safe to call more than once (a module reload re-runs it). */
void chkMpLiveRegister(JERICHO_CONTEXT* ctx);

#endif /* CHK_MPLIVE_H */
