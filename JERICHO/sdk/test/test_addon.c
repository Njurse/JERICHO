#include "jericho.h"
#include "jer_host_api.h"

/* The fake host fills the table; this addon proves the resolver works without
 * any engine import. */
JER_MODULE_ENTRY(jer_module_testaddon_entry)(JERICHO_CONTEXT* ctx)
{
	const JER_HOST_API* api = jerHostApiGet();

	(void)ctx;

	if (api == 0)
		return;

	api->core->log("testaddon resolved host api v%u abi=%08x\n", api->version, api->abi);

	if (api->car != 0)
		api->core->log("testaddon car count=%d\n", api->car->count());
}
