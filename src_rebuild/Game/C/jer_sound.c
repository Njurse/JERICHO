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

#include <stdlib.h>	/* malloc/free - the module bank file is read whole */
#include <string.h>	/* memcpy/strcmp */

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

typedef struct JerSoundLumpReq
{
	int used;
	int bank;
	int lump;
	int maxSamples;
	int base;	/* first sample index after load, -1 if none */
	int count;
} JerSoundLumpReq;

static JerSoundLumpReq gJerLump[JER_SOUND_LUMP_MAX];

int jer_sound_request_lump(int bank, int lump, int maxSamples)
{
	int i, freeSlot = -1;

	if (bank < 0 || lump < 0)
		return -1;

	for (i = 0; i < JER_SOUND_LUMP_MAX; i++)
	{
		if (!gJerLump[i].used)
		{
			if (freeSlot < 0)
				freeSlot = i;
			continue;
		}

		if (gJerLump[i].bank == bank && gJerLump[i].lump == lump)
		{
			/* a tighter cap wins; -1 means "the whole lump" and is the
			 * looser bound, so it never overwrites a real cap */
			if (maxSamples >= 0 && (gJerLump[i].maxSamples < 0 ||
				maxSamples < gJerLump[i].maxSamples))
				gJerLump[i].maxSamples = maxSamples;
			return i;
		}
	}

	if (freeSlot < 0)
		return -1;

	gJerLump[freeSlot].used = 1;
	gJerLump[freeSlot].bank = bank;
	gJerLump[freeSlot].lump = lump;
	gJerLump[freeSlot].maxSamples = maxSamples;
	gJerLump[freeSlot].base = -1;
	gJerLump[freeSlot].count = 0;
	return freeSlot;
}

int jer_sound_lump_base(int slot)
{
	if (slot < 0 || slot >= JER_SOUND_LUMP_MAX || !gJerLump[slot].used)
		return -1;
	return gJerLump[slot].base;
}

int jer_sound_lump_count(int slot)
{
	if (slot < 0 || slot >= JER_SOUND_LUMP_MAX || !gJerLump[slot].used)
		return 0;
	return gJerLump[slot].count;
}

static void JerLoadRequestedModBanks(void);

void jer_sound_load_requested_banks(void)
{
	int before;
	int i;

	gJerMenuBankBase = -1;
	gJerMenuBankCount = 0;

	if (gJerMenuBankWanted)
	{
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

	for (i = 0; i < JER_SOUND_LUMP_MAX; i++)
	{
		gJerLump[i].base = -1;
		gJerLump[i].count = 0;

		if (!gJerLump[i].used)
			continue;

		before = GetSoundBankSampleCount(gJerLump[i].bank);
		gJerLump[i].count = LoadBankFromLumpMax(gJerLump[i].bank, gJerLump[i].lump,
			gJerLump[i].maxSamples);

		if (gJerLump[i].count > 0)
			gJerLump[i].base = before;
		else
			gJerLump[i].count = 0;

		jer_log("[jer_sound] lump %d -> bank %d: %d sample(s) at %d (bank held %d of %d)\n",
			gJerLump[i].lump, gJerLump[i].bank, gJerLump[i].count, gJerLump[i].base,
			before, GetSoundBankCapacity());
	}

	JerLoadRequestedModBanks();
}

int jer_sound_menu_play(int sample)
{
	if (gJerMenuBankBase < 0 || sample < 0 || sample >= gJerMenuBankCount)
		return -1;

	return FESound(gJerMenuBankBase + sample);
}

/* ---- module sound banks (see jer_sound.h) ---------------------------- *
 * A module can ship a PSX dynamic bank beside its WAVs and have the engine
 * append it to a SOUND_BANK_* destination at every level load. The file is the
 * format LoadSoundBankDynamic() already reads:
 *
 *     int32 count, count x 16-byte (address, length, loop, rate), VAG payload
 *
 * The request is loaded in jer_sound_load_requested_banks(), i.e. BEFORE the
 * car-import memtop, so an imported car can never rewind over it. The sample
 * indices are only meaningful after that load: ask base/count then.
 */
#ifndef PSX

#define JER_BANK_ID_MAX 32

typedef struct JerSoundBankReq
{
	int used;
	char modId[JER_BANK_ID_MAX];
	char name[JER_BANK_ID_MAX];
	int bank;
	int base;	/* first sample index after load, -1 if none */
	int count;
} JerSoundBankReq;

static JerSoundBankReq gJerBank[JER_SOUND_BANK_MAX];

static int JerSoundFileExists(const char* path)
{
	FILE* f = fopen(path, "rb");

	if (f == NULL)
		return 0;

	fclose(f);
	return 1;
}

/* <root>/MODS/<modId>/sounds/<name><ext>: the repo tree first (a dev build),
 * then the shipped mirror beside the exe. Returns 1 and fills `out` when a file
 * is there. */
static int JerModAssetPath(const char* modId, const char* name, const char* ext,
			   char* out, int cap)
{
	const char* root = jer_root_dir();
	char rel[192];

	if (root == NULL || modId == NULL || name == NULL || ext == NULL ||
	    out == NULL || cap <= 0)
		return 0;

	snprintf(rel, sizeof(rel), "MODS/%s/sounds/%s%s", modId, name, ext);
	snprintf(out, (size_t)cap, "%s/../../../../JERICHO/%s", root, rel);

	if (JerSoundFileExists(out))
		return 1;

	snprintf(out, (size_t)cap, "%s/%s", root, rel);
	return JerSoundFileExists(out);
}

int jer_sound_request_mod_bank(const char* modId, const char* name, int bank)
{
	int i, freeSlot = -1;

	if (modId == NULL || name == NULL || modId[0] == 0 || name[0] == 0)
		return -1;

	if (bank < 0 || bank > SOUND_BANK_TANNER)
		return -1;

	for (i = 0; i < JER_SOUND_BANK_MAX; i++)
	{
		if (!gJerBank[i].used)
		{
			if (freeSlot < 0)
				freeSlot = i;
			continue;
		}

		if (gJerBank[i].bank == bank &&
		    strcmp(gJerBank[i].modId, modId) == 0 &&
		    strcmp(gJerBank[i].name, name) == 0)
			return i;
	}

	if (freeSlot < 0)
		return -1;

	gJerBank[freeSlot].used = 1;
	snprintf(gJerBank[freeSlot].modId, sizeof(gJerBank[freeSlot].modId),
		"%s", modId);
	snprintf(gJerBank[freeSlot].name, sizeof(gJerBank[freeSlot].name),
		"%s", name);
	gJerBank[freeSlot].bank = bank;
	gJerBank[freeSlot].base = -1;
	gJerBank[freeSlot].count = 0;
	return freeSlot;
}

int jer_sound_mod_bank_base(int slot)
{
	if (slot < 0 || slot >= JER_SOUND_BANK_MAX || !gJerBank[slot].used)
		return -1;

	return gJerBank[slot].base;
}

int jer_sound_mod_bank_count(int slot)
{
	if (slot < 0 || slot >= JER_SOUND_BANK_MAX || !gJerBank[slot].used)
		return 0;

	return gJerBank[slot].count;
}

/* Read one module bank file and append it to its destination. Runs at level
 * load, BEFORE the car-import memtop; a failure leaves base/count at -1/0 so
 * every caller keeps its stock sample. The file is the same layout
 * LoadSoundBankDynamic reads: int32 count, count x 16-byte entries, payload. */
static void JerLoadRequestedModBanks(void)
{
	int i;

	for (i = 0; i < JER_SOUND_BANK_MAX; i++)
	{
		char path[400];
		FILE* f;
		long size;
		unsigned char* buf;
		int count, payloadSize;
		long header;
		int before, got;
		int e, valid = 1;

		gJerBank[i].base = -1;
		gJerBank[i].count = 0;

		if (!gJerBank[i].used)
			continue;

		if (!JerModAssetPath(gJerBank[i].modId, gJerBank[i].name, ".sbk",
				     path, sizeof(path)))
		{
			jer_log("[jer_sound] mod bank %s/%s: missing\n",
				gJerBank[i].modId, gJerBank[i].name);
			continue;
		}

		f = fopen(path, "rb");

		if (f == NULL)
		{
			jer_log("[jer_sound] mod bank %s: unreadable\n", path);
			continue;
		}

		if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
		    fseek(f, 0, SEEK_SET) != 0)
		{
			fclose(f);
			jer_log("[jer_sound] mod bank %s: cannot size\n", path);
			continue;
		}

		if (size < (long)(sizeof(int) + 16) || size > 2L * 1024L * 1024L)
		{
			fclose(f);
			jer_log("[jer_sound] mod bank %s: bad size %ld\n", path, size);
			continue;
		}

		buf = (unsigned char*)malloc((size_t)size);

		if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size)
		{
			fclose(f);
			free(buf);
			jer_log("[jer_sound] mod bank %s: read failed\n", path);
			continue;
		}

		fclose(f);

		memcpy(&count, buf, sizeof(count));

		if (count <= 0 || count > GetSoundBankCapacity())
		{
			free(buf);
			jer_log("[jer_sound] mod bank %s: bad sample count %d\n",
				path, count);
			continue;
		}

		header = (long)sizeof(int) + (long)count * 16L;

		if (header > size)
		{
			free(buf);
			jer_log("[jer_sound] mod bank %s: truncated header\n", path);
			continue;
		}

		payloadSize = (int)size - (int)header;

		for (e = 0; e < count; e++)
		{
			unsigned int addr, length, loop, rate;

			memcpy(&addr, buf + sizeof(int) + e * 16 + 0, 4);
			memcpy(&length, buf + sizeof(int) + e * 16 + 4, 4);
			memcpy(&loop, buf + sizeof(int) + e * 16 + 8, 4);
			memcpy(&rate, buf + sizeof(int) + e * 16 + 12, 4);

			if (length == 0 || (length % 16) != 0 ||
			    addr > (unsigned)payloadSize ||
			    length > (unsigned)payloadSize - addr ||
			    loop > 1 || rate < 4000 || rate > 44100)
			{
				valid = 0;
				break;
			}
		}

		if (!valid)
		{
			free(buf);
			jer_log("[jer_sound] mod bank %s: invalid entry (payload %d)\n",
				path, payloadSize);
			continue;
		}

		if (!SoundBankHasRoom(gJerBank[i].bank, count))
		{
			free(buf);
			jer_log("[jer_sound] mod bank %s: bank %d has %d of %d samples, needs %d\n",
				path, gJerBank[i].bank,
				GetSoundBankSampleCount(gJerBank[i].bank),
				GetSoundBankCapacity(), count);
			continue;
		}

		if (payloadSize > SoundBankSpuRoom())
		{
			free(buf);
			jer_log("[jer_sound] mod bank %s: %d bytes of VAG, %d free\n",
				path, payloadSize, SoundBankSpuRoom());
			continue;
		}

		before = GetSoundBankSampleCount(gJerBank[i].bank);
		got = LoadSoundBankDynamic((char*)buf, (int)size, gJerBank[i].bank);

		if (got > 0)
		{
			gJerBank[i].base = before;
			gJerBank[i].count = got;
			jer_log("[jer_sound] mod bank %s -> bank %d: %d sample(s) at %d (bank held %d of %d)\n",
				path, gJerBank[i].bank, got, before,
				GetSoundBankSampleCount(gJerBank[i].bank),
				GetSoundBankCapacity());
		}
		else
		{
			jer_log("[jer_sound] mod bank %s: LoadSoundBankDynamic refused it\n",
				path);
		}

		free(buf);
	}
}

#else /* PSX */

static void JerLoadRequestedModBanks(void)
{
}

int jer_sound_request_mod_bank(const char* modId, const char* name, int bank)
{
	(void)modId;
	(void)name;
	(void)bank;
	return -1;
}

int jer_sound_mod_bank_base(int slot)
{
	(void)slot;
	return -1;
}

int jer_sound_mod_bank_count(int slot)
{
	(void)slot;
	return 0;
}

#endif /* PSX */

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

static JerWavEntry* JerWavFind(const char* modId, const char* name)
{
	char key[64], path[400];
	JerWavEntry* e;
	int i;

	snprintf(key, sizeof(key), "%s/%s", modId, name);

	for (i = 0; i < gJerWavCount; i++)
	{
		if (strcmp(gJerWav[i].key, key) == 0)
			return &gJerWav[i];
	}

	if (gJerWavCount >= JER_WAV_CACHE)
		return NULL;

	e = &gJerWav[gJerWavCount++];
	memset(e, 0, sizeof(*e));
	snprintf(e->key, sizeof(e->key), "%s", key);

	e->missing = 1;

	if (!JerModAssetPath(modId, name, ".wav", path, sizeof(path)))
	{
		jer_log("[jer_sound] mod wav %s/%s: missing\n", modId, name);
		return e;
	}

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
