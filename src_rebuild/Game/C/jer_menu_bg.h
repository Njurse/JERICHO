#ifndef JER_MENU_BG_H
#define JER_MENU_BG_H

/* jer_menu_bg — JERICHO's own frontend menu background.
 *
 * The frontend used to draw DATA/GFX.RAW and nothing else. With this on (the default) it
 * draws JERICHO/CORE/jericho_background.tga through jer_texture instead - the image is
 * scaled to the display, so any size works, and it is converted to the engine's colour
 * (see jer_texture.h).
 *
 * Turn it off with CONFIG/jericho.ini:
 *
 *     custom_menu_background = 0
 *
 * A missing or unreadable file is not an error: the stock background is drawn instead. */

/* Load the background if it should be shown and has not been loaded yet. Cheap once it
 * has run; call it from the screen draw. */
void jer_menu_bg_tick(void);

/* Non-zero when a custom background is loaded and should be drawn. */
int jer_menu_bg_ready(void);

/* The background texture (JER_TEX_NONE when there is none). */
unsigned int jer_menu_bg_texture(void);

#endif /* JER_MENU_BG_H */
