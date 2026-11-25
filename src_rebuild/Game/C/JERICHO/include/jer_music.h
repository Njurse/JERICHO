#ifndef JER_MUSIC_H
#define JER_MUSIC_H

/* jer_music - hold the level music on its calm or its pursuit track.
 *
 * Every level song has two parts: the calm cruise and the cop/pursuit section.
 * The engine switches between them through FunkUpDaBGMTunez (gamesnd.c): cop AI
 * picks the pursuit part when a chase starts and the calm part when the cops lose
 * you, and some missions start on the pursuit part (main.c). A module that just
 * called that function would be overridden by the next chase.
 *
 * jer_music_force pins the track instead. While a force is set, the engine's own
 * calls are remembered but not applied; releasing it (JER_MUSIC_AUTO) plays the
 * track the engine last asked for, so a chase that started under a force still
 * gets its pursuit music. With no force set nothing changes: stock felony
 * pursuit music behaves exactly as before.
 *
 * The engine releases the force itself on the way out of a level and on the
 * frontend entry, so a mode cannot leave the pursuit track stuck into the menus
 * or the next level. A module that wants it again re-applies it once in game.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define JER_MUSIC_AUTO		0	/* engine decides: cop AI / mission code */
#define JER_MUSIC_CALM		1	/* hold the calm part */
#define JER_MUSIC_PURSUIT	2	/* hold the pursuit part */

/* Set the force (JER_MUSIC_*; anything else is treated as AUTO). Takes effect
 * now: CALM/PURSUIT switch the track at once, AUTO restores the engine's choice. */
void jer_music_force(int mode);

/* The force currently set (JER_MUSIC_AUTO when none). */
int jer_music_forced(void);

#ifdef __cplusplus
}
#endif

#endif
