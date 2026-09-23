/* jer_sound.c — see JERICHO/include/jer_sound.h.
 *
 * The whole reserve rule is computed from the engine's own channel flags, so
 * there is no per-module bookkeeping to keep in sync: a voice counts as "taken"
 * the moment anything locks it (the engine at boot, or a module through here),
 * and jer_sound_lock refuses to add one when that would leave fewer than
 * JER_SFX_RESERVE unlocked. GetFreeChannel() therefore always has a voice to
 * hand the engine's own collision/explosion sounds.
 *
 * Engine-side (not in JERICHO/src) because it needs the game's channel table,
 * exactly like jer_hud.c needs the display buffer.
 */

#include "driver2.h"
#include "sound.h"		/* channels[], CHAN_LOCKED, LockChannel/UnlockChannel */
#include "jericho.h"
#include "jer_sound.h"

int jer_sound_locked_count(void)
{
	int i, n = 0;

	for (i = 0; i < MAX_SFX_CHANNELS; i++)
	{
		if (channels[i].flags & CHAN_LOCKED)
			n++;
	}

	return n;
}

int jer_sound_lock(int channel)
{
	if (channel < 0 || channel >= MAX_SFX_CHANNELS)
		return 0;

	// already locked (by us or the engine): nothing to do, and it IS held
	if (channels[channel].flags & CHAN_LOCKED)
		return 1;

	// keep JER_SFX_RESERVE voices reachable for the engine's own SFX
	if (jer_sound_locked_count() >= MAX_SFX_CHANNELS - JER_SFX_RESERVE)
		return 0;

	LockChannel(channel);

	return 1;
}

void jer_sound_unlock(int channel)
{
	if (channel < 0 || channel >= MAX_SFX_CHANNELS)
		return;

	UnlockChannel(channel);
}

int jer_sound_ensure(int* channel)
{
	if (channel == NULL)
		return -1;

	if (*channel < 0)
	{
		int c = GetFreeChannel(1);

		if (c < 0 || !jer_sound_lock(c))
		{
			*channel = -1;
			return -1;		/* play unlocked */
		}

		*channel = c;
	}
	else if (!jer_sound_lock(*channel))
	{
		// our cached voice is no longer locked (ResetSound clears every lock at
		// a level change) and re-locking it would eat the reserve: drop it and
		// play unlocked rather than hold a voice we do not own
		*channel = -1;
		return -1;
	}

	return *channel;
}

int jer_sound_channel_busy(int channel)
{
	if (channel < 0 || channel >= MAX_SFX_CHANNELS)
		return 0;

	return (channels[channel].time != 0) ? 1 : 0;
}
