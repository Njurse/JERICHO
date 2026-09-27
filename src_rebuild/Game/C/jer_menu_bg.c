// jer_menu_bg.c — JERICHO's own frontend menu background (see jer_menu_bg.h).
//
// The frontend draws this instead of the stock DATA/GFX.RAW art, unless the option is
// switched off or the file will not load. It goes through jer_texture like any other
// custom texture, so it is converted to the engine's colour and capped to a sane size on
// the way in - the file on disk can be any TGA of any size.

#include "driver2.h"
#include "jericho.h"
#include "JERICHO/include/jer_config.h"
#include "JERICHO/include/jer_texture.h"
#include "jer_menu_bg.h"

#include <stdio.h>

#define JER_MENU_BG_FILE	"jericho_background.tga"
#define JER_MENU_BG_MOD		"jericho"
#define JER_MENU_BG_OPTION	"custom_menu_background"

static JER_TEXTURE gMenuBg = JER_TEX_NONE;
static int gMenuBgTried = 0;

int jer_menu_bg_ready(void)
{
	return gMenuBg != JER_TEX_NONE;
}

unsigned int jer_menu_bg_texture(void)
{
	return (unsigned int)gMenuBg;
}

void jer_menu_bg_tick(void)
{
	char path[JER_TEX_NAME_MAX * 4];

	if (gMenuBgTried)
		return;

	// OFF means never load it - but do not mark the attempt done, so turning the option
	// back on (the mods menu rewrites the ini and re-inits the config) takes effect.
	if (!jer_config_get_bool(JER_MENU_BG_MOD, JER_MENU_BG_OPTION, 1))
		return;

	gMenuBgTried = 1;

	if (!jer_texture_core_path(JER_MENU_BG_FILE, path, sizeof(path)))
		return;

	gMenuBg = jer_texture_load_path(path, JER_TEX_TARGET_IMAGE);

	jer_log("jer_menu_bg: %s -> texture %u\n", path, (unsigned)gMenuBg);
}
