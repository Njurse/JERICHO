/* test_registry.c — standalone registry for the JERICHO smoke test.
 * Lists only modules that link without game globals (example). */
#include "jericho.h"

#if defined(__cplusplus)
extern "C" {
#endif
void jer_module_example_entry(JERICHO_CONTEXT* ctx);

extern const JER_REGISTRY_ENTRY jer_registry_modules[] = {
	{ "example", jer_module_example_entry, 0 },
};
extern const int jer_registry_module_count = 1;
#if defined(__cplusplus)
}
#endif
