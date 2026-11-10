#ifndef JER_REBAKE_H
#define JER_REBAKE_H

/* jer_rebake -- the one-shot "re-bake your Driver 1 cars?" prompt.
 *
 * It asks on the main menu when the bake is older than the tooling, waits for
 * the mod store's startup update question, and can be re-armed after a mod
 * install brings newer tooling. "Don't ask again" (CONFIG/rebake.ini
 * bake_prompt=0) turns the startup questions off. jer_modstore.c calls the re-arm; FEmain.c calls
 * the tick. */

void jer_rebake_frontend_tick(int onMainMenu);

/* Non-zero while the boot's startup questions (the mod store's update question,
 * then the bake one) are not settled yet. FEmain.c keeps the main menu hidden
 * and off the pad meanwhile, so a question is never preceded by a flash of the
 * menu. Once released it stays released for the run. */
int jer_rebake_boot_hold(void);

/* Let the next quiet main-menu frame ask again (one-shot per run otherwise). */
void jer_rebake_rearm(void);

/* What Yes on those questions does: run BAKE_D1_CARS --from-game and exit so
 * the script can relaunch the game. Options -> JERICHO -> Bake calls this too. */
void jer_rebake_start(void);

#endif /* JER_REBAKE_H */
