#ifndef PEDHACKS_H
#define PEDHACKS_H

/* pedhacks - the JMPPED pedestrian asset module.
 *
 * This is the module boundary for the pedestrian archive: archive lifecycle,
 * character entries, model/page/palette installation. The implementation
 * starts as a compiled-in module and moves off the engine headers as the
 * interface-3 pedestrian contract lands.
 */

#include "jericho.h"

#ifdef __cplusplus
extern "C" {
#endif

void pedhacks_register(JERICHO_CONTEXT* ctx);

#ifdef __cplusplus
}
#endif

#endif /* PEDHACKS_H */
