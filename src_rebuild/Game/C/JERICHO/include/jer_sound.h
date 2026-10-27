#ifndef JER_SOUND_H
#define JER_SOUND_H

/* jer_sound — module-safe sound-channel management.
 *
 * The engine has MAX_SFX_CHANNELS (16) SPU voices. It locks voices 0..2 for
 * itself at boot (0..5 in two-player), and its OWN one-shot effects — the
 * collision bang, explosions, tyre screech — are played on whatever
 * GetFreeChannel() hands out. A module that LOCKS a voice for each of its own
 * sounds can therefore leave the engine with none: once every remaining voice is
 * locked, GetFreeChannel() returns -1 and the crash you just had makes no sound
 * at all.
 *
 * jer_sound_lock() is the fix. It locks a voice only while at least
 * JER_SFX_RESERVE stay free for the engine, so a module can never starve the
 * generic SFX pool. A REFUSED lock is not an error: play the sound unlocked (pass
 * -1 to Start3DSoundVolPitch, which picks a free voice) instead of holding one.
 *
 * Typical use, replacing a raw GetFreeChannel(1)/LockChannel pair:
 *
 *     if (gMyChannel < 0)
 *     {
 *         int c = GetFreeChannel(1);
 *         if (c >= 0 && jer_sound_lock(c))
 *             gMyChannel = c;
 *     }
 *     Start3DSoundVolPitch(gMyChannel, ...);   // -1 = a free voice, unlocked
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "jericho.h"

/* Voices always left free for the engine's own SFX. */
#define JER_SFX_RESERVE 4

/* Lock `channel` for a module, unless doing so would eat into the reserve.
 * Returns 1 when the channel is now locked, 0 when refused (or the channel is
 * out of range / already the engine's) — in that case play the sound on a free
 * voice instead. */
int jer_sound_lock(int channel);

/* The one-call form of the pattern above, for a cached channel variable that is
 * -1 until first use. Acquires and locks a voice when *channel < 0, re-locks it
 * if the engine dropped the lock (ResetSound clears every lock at a level
 * change), and gives up gracefully when the reserve is reached. RETURNS the
 * channel to play on, or -1 to play on a free voice unlocked. */
int jer_sound_ensure(int* channel);

/* Release a channel (one locked with jer_sound_lock, or any other). */
void jer_sound_unlock(int channel);

/* Release EVERY locked voice. A module's lock survives a level change (ResetSound
 * clears them at the next level's start) but NOT the return to the menu, so a
 * module that held a voice for the whole match keeps it out of the pool while the
 * menus are up - the engine's own SFX then get refused. Called on the frontend
 * entry, after StopAllChannels, so the pool the menus leave behind is the pool a
 * fresh session starts with. */
void jer_sound_release_all(void);

/* 1 while the channel is still playing — use it to release an idle cached
 * channel rather than hold a voice for a sound that has finished. */
int jer_sound_channel_busy(int channel);

/* How many voices are currently locked (the engine's included), for logs/tests. */
int jer_sound_locked_count(void);

/* ---- frontend (menu) samples in game -----------------------------------
 * The FE menu bank (VOICES2.BLK lump 0) is only resident in the frontend. A
 * module that wants those samples in game (placeholder SFX, say) asks for
 * them once at init; every following level load appends those four samples to the
 * SFX bank, BEFORE the car-sound memtop is saved, so car imports never rewind
 * over it. Nothing is loaded unless a module asked. */
#define JER_MENU_SFX_BACK	0	/* Triangle / back */
#define JER_MENU_SFX_MOVE	1	/* cursor step */
#define JER_MENU_SFX_ACCEPT	2	/* Cross / accept */
#define JER_MENU_SFX_MOVE2	3	/* cursor step, other direction */
#define JER_MENU_SFX_COUNT	4	/* only these are loaded in game */

void jer_sound_request_menu_bank(void);

/* Play a menu sample (JER_MENU_SFX_*) on a free voice, non-positional.
 * Returns the channel, or -1 when the bank is not resident or no voice. */
int jer_sound_menu_play(int sample);

/* Play <root>/MODS/<modId>/sounds/<name>.wav (the repo copy first, as textures)
 * once, non-positional, at the master volume. A loose mod-folder file, not a LEV
 * bank: optional assets a module can fall back from. Returns 1 when it played, 0
 * when the file is absent or unreadable (cached: a missing file costs one look).
 * Desktop builds only; always 0 on PSX. */
int jer_sound_mod_wav_play(const char* modId, const char* name);

/* engine-side: LoadLevelSFX calls this before saving the memtop */
void jer_sound_load_requested_banks(void);

#ifdef __cplusplus
}
#endif

#endif /* JER_SOUND_H */
