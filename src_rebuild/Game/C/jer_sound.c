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
#include "gamesnd.h"	/* LoadBankFromLump, SOUND_BANK_SFX, SBK_ID_MENU */

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

/* Drop every lock, the engine's own included: ResetSound clears them all at a level
 * change and main.c re-locks the three it reserves, so a blanket clear here is the
 * same state a fresh level starts from, and it is the only thing that can recover a
 * voice a module locked and never gave back. */
void jer_sound_release_all(void)
{
	int i, n = 0;

	for (i = 0; i < MAX_SFX_CHANNELS; i++)
	{
		if (channels[i].flags & CHAN_LOCKED)
			n++;

		UnlockChannel(i);
	}

	jer_log("[jer_sound] frontend entry: %d locked voice(s) released (%d/%d held after)\n",
		n, jer_sound_locked_count(), MAX_SFX_CHANNELS);
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

/* ---- frontend samples in game (see jer_sound.h) ---------------------- */

static int gJerMenuBankWanted = 0;
static int gJerMenuBankBase = -1;
static int gJerMenuBankCount = 0;

void jer_sound_request_menu_bank(void)
{
	gJerMenuBankWanted = 1;
}

void jer_sound_load_requested_banks(void)
{
	int before;

	gJerMenuBankBase = -1;
	gJerMenuBankCount = 0;

	if (!gJerMenuBankWanted)
		return;

	/* Only the four JER_MENU_SFX_* samples are needed. Loading the whole
	 * frontend lump overflowed the in-game SFX bank, which refused it
	 * outright, so every jer_sound_menu_play was silently a no-op. */
	before = GetSoundBankSampleCount(SOUND_BANK_SFX);
	gJerMenuBankCount = LoadBankFromLumpMax(SOUND_BANK_SFX, SBK_ID_MENU, JER_MENU_SFX_COUNT);

	if (gJerMenuBankCount > 0)
		gJerMenuBankBase = before;
	else
		gJerMenuBankCount = 0;

	jer_log("[jer_sound] menu samples in game: %d at SFX %d (bank held %d of %d)\n",
		gJerMenuBankCount, gJerMenuBankBase, before, GetSoundBankCapacity());
}

int jer_sound_menu_play(int sample)
{
	if (gJerMenuBankBase < 0 || sample < 0 || sample >= gJerMenuBankCount)
		return -1;

	return FESound(gJerMenuBankBase + sample);
}

/* ------------------------------------------------------------------ */
/* Loose mod-folder WAVs (jer_sound_mod_wav_play)                       */
/* ------------------------------------------------------------------ */

#ifndef PSX
#include "../utils/audio_source/snd_al_source.h"
#include "../utils/audio_source/snd_wav_cache.h"
#include "AL/al.h"

#define JER_WAV_CACHE	16
#define JER_WAV_VOICES	4

typedef struct JerWavEntry
{
	char key[64];				/* "<modId>/<name>" */
	int missing;				/* looked once, not there */
	CSoundSource_WaveCache* wav;
	CSoundSource_OpenALCache* al;
} JerWavEntry;

static JerWavEntry gJerWav[JER_WAV_CACHE];
static int gJerWavCount = 0;
static ALuint gJerWavVoice[JER_WAV_VOICES];
static int gJerWavVoicesUp = 0;
static int gJerWavNext = 0;

static int JerWavExists(const char* path)
{
	FILE* f = fopen(path, "rb");

	if (f == NULL)
		return 0;

	fclose(f);
	return 1;
}

static JerWavEntry* JerWavFind(const char* modId, const char* name)
{
	char key[64], rel[160], path[400];
	const char* root = jer_root_dir();
	JerWavEntry* e;
	int i;

	snprintf(key, sizeof(key), "%s/%s", modId, name);

	for (i = 0; i < gJerWavCount; i++)
	{
		if (strcmp(gJerWav[i].key, key) == 0)
			return &gJerWav[i];
	}

	if (gJerWavCount >= JER_WAV_CACHE || root == NULL)
		return NULL;

	e = &gJerWav[gJerWavCount++];
	memset(e, 0, sizeof(*e));
	snprintf(e->key, sizeof(e->key), "%s", key);

	/* the repo's MODS tree first (dev build), then the shipped mirror */
	snprintf(rel, sizeof(rel), "MODS/%s/sounds/%s.wav", modId, name);
	snprintf(path, sizeof(path), "%s/../../../../JERICHO/%s", root, rel);

	if (!JerWavExists(path))
		snprintf(path, sizeof(path), "%s/%s", root, rel);

	e->missing = 1;

	if (JerWavExists(path))
	{
		e->wav = new CSoundSource_WaveCache();

		if (e->wav->Load(path))
		{
			e->al = new CSoundSource_OpenALCache(e->wav);
			e->missing = 0;
			jer_log("[jer_sound] mod wav %s\n", path);
		}
		else
		{
			delete e->wav;
			e->wav = NULL;
			jer_log("[jer_sound] mod wav %s: unreadable\n", path);
		}
	}

	return e;
}

int jer_sound_mod_wav_play(const char* modId, const char* name)
{
	JerWavEntry* e;
	ALuint src;

	if (modId == NULL || name == NULL)
		return 0;

	e = JerWavFind(modId, name);

	if (e == NULL || e->missing || e->al == NULL)
		return 0;

	if (!gJerWavVoicesUp)
	{
		int i;

		alGenSources(JER_WAV_VOICES, gJerWavVoice);

		for (i = 0; i < JER_WAV_VOICES; i++)
		{
			alSourcei(gJerWavVoice[i], AL_LOOPING, 0);
			alSourcei(gJerWavVoice[i], AL_SOURCE_RELATIVE, AL_TRUE);
		}

		gJerWavVoicesUp = 1;
	}

	src = gJerWavVoice[gJerWavNext];
	gJerWavNext = (gJerWavNext + 1) % JER_WAV_VOICES;

	alSourceStop(src);
	alSourcei(src, AL_BUFFER, e->al->m_alBuffer);
	alSourcef(src, AL_GAIN, (float)((10000 + gMasterVolume) / 79) / 128.0f);
	alSourcePlay(src);

	return 1;
}
#else
int jer_sound_mod_wav_play(const char* modId, const char* name)
{
	(void)modId;
	(void)name;
	return 0;
}
#endif
