/* test_registry.c — standalone registry for the JERICHO smoke test.
 * Zero compiled-in modules: the test exercises the runtime DLL loader
 * (jer_loader.c) against a sandbox JERICHO/MODS folder. */
#include "jericho.h"

#if defined(__cplusplus)
extern "C" {
#endif
extern const JER_REGISTRY_ENTRY jer_registry_modules[] = {
	{ NULL, NULL, 0 },
};
extern const int jer_registry_module_count = 0;
#if defined(__cplusplus)
}
#endif
