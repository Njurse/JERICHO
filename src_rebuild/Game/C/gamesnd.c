

#include <stdlib.h>

#include "driver2.h"
#include "jericho.h"	// JERICHO-HOOK: mod runtime
#include "jer_events.h"	// JERICHO-HOOK: event argument structs
#include "gamesnd.h"

#include "sound.h"
#include "system.h"
#include "handling.h"
#include "mission.h"
#include "models.h"		// JERICHO: embedded D1 car-sound banks
#include "mc_snd.h"
#include "glaunch.h"
#include "loadview.h"
#include "cutscene.h"
#include "camera.h"
#include "cop_ai.h"
#include "cars.h"
#include "wheelforces.h"
#include "envsound.h"
#include "convert.h"
#include "xmplay.h"
#include "players.h"
#include "main.h"
#include "sky.h"
#include "debris.h"
#include "felony.h"

typedef struct __othercarsound
{
	int car;
	int chan;
	char in_use;
	char stopped;
	char idle;
} othercarsound;

typedef struct __bitfield64
{
	int h;
	int l;
} bitfield64;

typedef struct __envsound
{
	u_char type;
	u_char flags;
	VECTOR pos;
	VECTOR pos2;
	int bank;
	int sample;
	int vol;
} envsound;

typedef struct __envsoundtags
{
	int frame_cnt;
	int func_cnt;
	int num_envsnds;
	int envsnd_cnt;
} envsoundtags;

typedef struct __envsoundinfo
{
	VECTOR eff_pos[4];
	VECTOR cam_pos;
	float g[4];
	int thisS[4];
	int playing_sound[4];
	int chan[4];
	u_int flags;
} envsoundinfo;

typedef void(*envsoundfunc)(envsound* ep /*$s1*/, envsoundinfo* E /*$a1*/, int pl /*$a2*/);

void IdentifyZone(envsound* ep, envsoundinfo* E, int pl);
void CalcEffPos(envsound* ep, envsoundinfo* E, int pl);
void CalcEffPos2(envsound* ep, envsoundinfo* E, int pl);
void UpdateEnvSnd(envsound* ep, envsoundinfo* E, int pl);

static envsoundfunc UpdateEnvSounds[] = {
	IdentifyZone,
	CalcEffPos,
	CalcEffPos2,
	UpdateEnvSnd
};

static envsoundtags EStags;

struct GEAR_DESC
{
	int lowidl_ws;
	int low_ws;
	int hi_ws;
	int ratio_ac;
	int ratio_id;
};

GEAR_DESC geard[2][4] =
{
	{
		{ 0, 0, 163, 144, 135 },
		{ 86, 133, 260, 90, 85 },
		{ 186, 233, 360, 60, 57 },
		{ 286, 326, 9999, 48, 45 }
	},
	{
		{ 0, 0, 50, 144, 135 },
		{ 43, 66, 100, 90, 85 },
		{ 93, 116, 150, 60, 57 },
		{ 143, 163, 9999, 48, 45 }
	}
};

// XM song position
int xm_coptrackpos_d2[8] = {
	0x10, 0xB, 7, 0x12, 0xC, 9, 8, 0xA,
};

int xm_coptrackpos_d1[8] = {
	0xB, 0xC, 0xB, 0xB, 8, 0xB, 0xF, 0xC,
};

int* xm_coptrackpos;
int gDriver1Music = 0;

SPEECH_QUEUE gSpeechQueue;
static char cop_bank = 0;
char phrase_top = 0;

static othercarsound siren_noise[MAX_SIREN_NOISES];
static int siren_noise_sample[MAX_SIREN_NOISES];	// packed CarHasSiren value; 0 = music/none
static unsigned car_zero_siren_reported;	// one-shot log for a flagged car with no siren sample
static unsigned imported_siren_reported;	// one-shot log for an imported slot using a siren
static unsigned civilian_import_reported;	// one-shot log for a suppressed civilian-import siren
static othercarsound car_noise[MAX_CAR_NOISES];

/* JERICHO cross-city imports: the SOUND_BANK_CARS sample block each resident
 * slot's car actually lives in. The stock GetCarBankSample() derives a block
 * from the slot index, which only works because LoadLevelSFX loads banks in
 * that same order. An imported slot is appended AFTER the stock and special
 * banks, so its block cannot be derived from its index -- record it here when
 * the bank is loaded. -1 = use the stock formula.
 *
 * city/model are kept with the block so a slot that is released and reused (a
 * pick changed, a peer's car freed) notices it must load a different bank. */
static int car_slot_sample[MAX_CAR_RESIDENT_MODELS];
static int car_slot_city[MAX_CAR_RESIDENT_MODELS];
static int car_slot_model[MAX_CAR_RESIDENT_MODELS];
static int car_slot_imported[MAX_CAR_RESIDENT_MODELS];	// 1 = slot owns an imported per-model bank
static int car_slot_appended[MAX_CAR_RESIDENT_MODELS];	// 1 = slot's block was appended AFTER the level checkpoint
static int car_sound_ready;
static int car_sound_checkpoint_ready;	// the post-special bank checkpoint can be restored
static int car_sound_rebuilding;	// a rebuild is re-loading the live slots (no recursion)
static int loudhail_time = 0;

/* JERICHO-DIAG: one line per horn/siren sample actually started, so the car
 * stress harness can prove the same car never uses one sample for both. Off
 * unless JERICHO_DIAG_SIREN is set; normal runs pay one getenv call. */
void SoundLogCarSample(const char* kind, int slot, int sample)
{
	int city;
	int model;

	if (getenv("JERICHO_DIAG_SIREN") == NULL)
		return;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return;

	city = GetCarModelSourceCity(slot);
	if (city < 0 || city >= CITY_COUNT)
		city = GameLevel;
	if (city < 0 || city >= CITY_COUNT)
		city = 0;

	model = residentCarModels[slot];

	printInfo("gamesnd: %s: slot %d (%s model %d) sample %d\n",
		kind, slot, LevelNames[city], model, sample);
}

// JERICHO: an imported car that the bake marked as a cop gets Driver 2's siren
// treatment. The imported bank's third sample is the city's siren sample (see
// add_car_sounds.py --city), so return a packed (SOUND_BANK_CARS, sample) siren
// value; CarHasSiren then drives the same third channel, horn on/off and AI
// pursuer loop as a D2 cop. It returns 0 for EVERY import the bake did not mark
// (a civilian), and a caller must not fall back to the stock model-0 rule for
// an imported slot - that fallback is only for native/D2 slots.
static int ImportedCarHasSiren(int slot)
{
	int city, model;

	if (!car_sound_ready)
		return 0;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return 0;

	if (!car_slot_imported[slot])
		return 0;

	city = car_slot_city[slot];
	model = car_slot_model[slot];

	if (city < 0 || model < 0)
		return 0;

	if (!GetCarImportSiren(city, model) || car_slot_sample[slot] < 0)
		return 0;

	if (!(imported_siren_reported & (1u << slot)))
	{
		imported_siren_reported |= 1u << slot;
		printInfo("cross-city: slot %d (%s model %d) uses the imported siren (bank sample %d)\n",
			slot, LevelNames[city], model, car_slot_sample[slot] * 3 + 2);
	}

	return M_SHRT_2(SOUND_BANK_CARS, car_slot_sample[slot] * 3 + 2);
}

static int copmusic = 0;
int current_music_id;

static char header_pt[sizeof(XMHEADER)];
static char song_pt[sizeof(XMSONG)];

static envsound envsnd[MAX_LEVEL_ENVSOUNDS];
static envsoundinfo ESdata[2];
tunnelinfo tunnels;

// [D] [T]
void LoadBankFromLump(int bank, int lump)
{
	static u_int blockLimit[73] = { 0 };

	int size;
	char* name;

	name = "SOUND\\VOICES2.BLK";

	if (blockLimit[1] == 0)
		LoadfileSeg(name, (char*)blockLimit, 0, sizeof(blockLimit));

	size = blockLimit[lump + 1] - blockLimit[lump];
	if(size <= 0)
	{
		printWarning("WARNING: sound bank lump %d is not valid!\n", lump);
		return;
	}
	
	LoadfileSeg(name, (char*)_sbank_buffer, blockLimit[lump], size);
	if (size > 0 && blockLimit[lump] && blockLimit[lump + 1]) // [A]
	{
		LoadSoundBankDynamic((char*)_sbank_buffer, size, bank);
	}
	else
		printWarning("WARNING: sound bank lump %d is not valid!\n", lump);
}

// [D] [T]
/* `slot` is a resident car-slot index - the value carried in cp->ap.model -
 * not the car-model number stored in residentCarModels[]. The result is a
 * packed (bank, sample) SIREN, not the car bank's generic third sample. */
int CarHasSiren(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return 0;

	if (slot == SPECIAL_CAR_SLOT)
	{
		if (GameLevel == 0)
		{
			if (residentCarModels[SPECIAL_CAR_SLOT] == 8)
				return M_SHRT_2(SOUND_BANK_SFX, 12);
		}
		else if (GameLevel == 2)
		{
			if (residentCarModels[SPECIAL_CAR_SLOT] == 9)
				return M_SHRT_2(SOUND_BANK_SFX, 12);
		}
		else if (GameLevel == 3)
		{
			if (residentCarModels[SPECIAL_CAR_SLOT] == 10)
				return M_SHRT_2(SOUND_BANK_SFX, 12);
		}

		return M_SHRT_2((residentCarModels[SPECIAL_CAR_SLOT] == 0) ? SOUND_BANK_VOICES : 0, 0);
	}

	/* A Driver 1/custom import keeps its SOURCE city's model numbering, so the
	 * stock "model 0 is the D2 cop" rule does not apply: the bake's siren mask is
	 * the only authority. Returning the imported answer directly also returns 0
	 * for a civilian import instead of falling through to the stock siren. */
	if (GetCarModelSourceCity(slot) >= CITY_D2_COUNT || car_slot_imported[slot])
	{
		int city = GetCarModelSourceCity(slot);
		int imported = ImportedCarHasSiren(slot);

		/* This is the exact case the old fall-through turned into a siren. It is
		 * a civilian in its own city, so say once that the stock model-0 siren
		 * was suppressed rather than silently returning 0. */
		if (imported == 0 && residentCarModels[slot] == 0 && !(civilian_import_reported & (1u << slot)))
		{
			civilian_import_reported |= 1u << slot;
			printInfo("gamesnd: slot %d (%s model 0) is a civilian import - no siren\n",
				slot, (city >= 0 && city < CITY_COUNT) ? LevelNames[city] : "?");
		}

		return imported;
	}

	return M_SHRT_2((residentCarModels[slot] == 0) ? SOUND_BANK_VOICES : 0, 0);
}

// [D] [T]
int SpecialVehicleKludge(char vehicle2)
{
	static char kludge_bank[4][3] = {
		{ 2, 18, 10 },
		{ 2, 17, 4 },
		{ 9, 11, 13 },
		{ 17, 11, 15 }
	};

	return kludge_bank[GameLevel][vehicle2];
}

// [D] [T]
int ResidentModelsBodge(void)
{
	int i;
	int j;

	if (gCurrentMissionNumber == 24 ||
		gCurrentMissionNumber == 27 ||
		gCurrentMissionNumber == 29 ||
		gCurrentMissionNumber == 30 ||
		gCurrentMissionNumber == 35)
	{
		return 3;
	}

	j = residentCarModels[SPECIAL_CAR_SLOT];

	if (gCurrentMissionNumber - 50U < 16 && j == 12)
	{
		return 5;
	}

	if (GameLevel == 0)
	{
		i = 11;

		if (j != 9 && j != i)
			return 3;
	}
	else if (GameLevel == 1)
	{
		if (j - 8U > 1)
			return 3;
	}
	else if (GameLevel == 2)
	{
		i = 8;

		if (j != i)
			return 3;
	}
	else if (GameLevel == 3)
	{
		i = 11;

		if (j != i)
			return 3;
	}
	else
		return 3;

	return 4;
}

// [A]
int GetCarBankSample(int slot)
{
	int bankStartSample;

	if (slot == SPECIAL_CAR_SLOT)
	{
#if MAX_CAR_RESIDENT_MODELS > 5
		bankStartSample = ResidentModelsBodge() + 1;
#else
		bankStartSample = ResidentModelsBodge();
#endif
	}
	else if (car_sound_ready && slot >= 0 && slot < MAX_CAR_RESIDENT_MODELS &&
		car_slot_sample[slot] >= 0)
	{
		// A bank appended after the stock ones: its block was recorded when it
		// was loaded, so it cannot be derived from the slot index.
		bankStartSample = car_slot_sample[slot];
	}
	else if (slot < 3)
		bankStartSample = slot;
	else
		bankStartSample = slot - 1;

	return bankStartSample;
}

// The four Driver 2 cities' car-sound tables, indexed by model number. Only
// these cities have per-model engine/idle/horn banks in VOICES2.BLK; a
// transplanted Driver 1 city or a runtime bank has none of its own.
static const char car_banks[4][9] =
{
	{12, 4, 13, 5, 16, 9, 2, 15, 10},
	{10, 11, 7, 6, 17, 17, 2, 5, 4},
	{14, 3, 4, 6, 11, 9, 2, 17, 10},
	{8, 7, 13, 9, 2, 17, 17, 11, 16},
};

// The bank lump carrying a car's engine, idle and horn samples. `city` is the
// city whose car-data table `model` belongs to - which is NOT always GameLevel,
// because a cross-city import keeps the model number of the city it came from.
//
// A city without a table (Driver 1's transplanted cities, a runtime bank)
// falls back to the host level's row: the sound for the same model number is
// not ideal, but an engine and an idle loop beat a silent car.
static int MapCarModelToBank(int city, int model)
{
	int ret;

	if (city < 0 || city >= (int)(sizeof(car_banks) / sizeof(car_banks[0])))
		city = GameLevel;

	if (city < 0 || city >= (int)(sizeof(car_banks) / sizeof(car_banks[0])))
		city = 0;

	ret = model - 1;

	if (model == 0)
		ret = 1;

	if (ret > 6)
		ret -= 3;

	// The callers clamp model 13 (the special sentinel) already; keep a bad
	// model number from walking off the row rather than faulting.
	if (ret < 0)
		ret = 0;

	if (ret > 8)
		ret = 8;

	return car_banks[city][ret];
}

// [D] [T]
int MapCarIndexToBank(int index)
{
	int* RM;
	int model;
	int city;

	RM = residentCarModels;

	model = RM[index];

	if (gCurrentMissionNumber == 40 && RM[index] == 13)
	{
		model = 10 - (RM[0] + RM[1] + RM[2]);

		if (model < 1)
			model = 1;

		if (model > 5)
			model = 5;
	}

	// A slot imported from another city keeps that city's model number, so its
	// sound has to be looked up in THAT city's table. Native slots (source -1)
	// stay exactly on GameLevel, which is the stock path.
	city = GetCarModelSourceCity(index);

	if (city < 0)
		city = GameLevel;

	return MapCarModelToBank(city, model);
}

/* Restart any looping car-noise channel a slot already owns, so a bank that
 * arrives mid-level is used on the next SoundTasks tick instead of leaving the
 * old (stand-in) car's engine loop running. */
static void RestartCarNoiseForSlot(int slot)
{
	int j;

	for (j = 0; j < MAX_CAR_NOISES; j++)
	{
		if (!car_noise[j].in_use || car_noise[j].car < 0 || car_noise[j].car >= MAX_CARS)
			continue;

		if (car_data[car_noise[j].car].ap.model != slot)
			continue;

		StopChannel(car_noise[j].chan);
		UnlockChannel(car_noise[j].chan);

		car_noise[j].chan = -1;
		car_noise[j].car = MAX_CARS;
		car_noise[j].stopped = 1;
		car_noise[j].in_use = 0;
	}
}

/* A slot's bank just changed under a live siren. Stop the AI siren entry that
 * belongs to that resident slot and let DoDopplerSFX() re-issue it; for a local
 * player whose car is on the slot, stop/restart the per-player siren channel
 * now with the new packed sample. DoDopplerSFX's sample comparison is the
 * safety net when this runs before ap.model has been re-pointed. */
static void RestartSirenForSlot(int slot)
{
	int i;

	for (i = 0; i < MAX_SIREN_NOISES; i++)
	{
		int car = siren_noise[i].car;

		if (siren_noise[i].stopped != 0 || car < 0 || car >= MAX_CARS)
			continue;

		if (car_data[car].ap.model != slot)
			continue;

		if (siren_noise[i].chan >= 0)
		{
			StopChannel(siren_noise[i].chan);
			UnlockChannel(siren_noise[i].chan);
		}

		siren_noise[i].chan = -1;
		siren_noise[i].car = MAX_CARS;
		siren_noise[i].in_use = 0;
		siren_noise[i].stopped = 1;
		siren_noise_sample[i] = 0;
	}

	for (i = 0; i < NumPlayers; i++)
	{
		int carId = player[i].playerCarId;
		int siren;
		int chan;

		if (player[i].playerType != PLAYER_TYPE_CAR || carId < 0 || carId >= MAX_CARS)
			continue;

		if (car_data[carId].ap.model != slot)
			continue;

		chan = i * 3 + 2;
		siren = CarHasSiren(slot);

		StopChannel(chan);

		if (siren)
		{
			CAR_DATA* cp = &car_data[carId];

			Start3DSoundVolPitch(chan, (siren & 0xff00) >> 8, siren & 0xff,
				cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], -10000, 129);
		}
	}
}

/* Try a city's embedded (Driver 1 bake) bank for `slot` before the host-city
 * fallback. The blob is already a D2 dynamic bank, so this only has to align,
 * load and record the block. Returns 1 when it took over the slot. */
static int LoadEmbeddedCarSoundForSlot(int slot)
{
	int city, model, before, after;
	char* bank;
	int size;

	if (!car_sound_ready)
		return 0;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS || slot == SPECIAL_CAR_SLOT)
		return 0;

	city = GetCarModelSourceCity(slot);
	model = residentCarModels[slot];

	if (city < 0 || model < 0)
		return 0;

	if (car_slot_sample[slot] >= 0 &&
		car_slot_city[slot] == city && car_slot_model[slot] == model)
		return 1;

	if (!GetCarImportSoundBank(city, model, &bank, &size))
		return 0;

	// Per-model banks: every imported model carries its own engine, idle and
	// horn, so load this slot's model bank instead of sharing one generic block.
	AlignSoundBankSamples(SOUND_BANK_CARS, 3);

	before = GetSoundBankSampleCount(SOUND_BANK_CARS);
	car_slot_sample[slot] = -1;

	if (LoadSoundBankDynamic(bank, size, SOUND_BANK_CARS) <= 0)
		return 0;

	after = GetSoundBankSampleCount(SOUND_BANK_CARS);

	if (after <= before)
		return 0;

	car_slot_sample[slot] = before / 3;
	car_slot_city[slot] = city;
	car_slot_model[slot] = model;
	car_slot_imported[slot] = 1;

	printInfo("gamesnd: resident slot %d uses the imported model bank (%s model %d) -> block %d\n",
		slot, LevelNames[city], model, car_slot_sample[slot]);

	RestartCarNoiseForSlot(slot);
	return 1;
}

/* JERICHO: the player engine/idle channels play a block recorded at start-up;
 * a rebuild can move that block, so re-issue them from the cars on the road. */
static void RestartPlayerCarSoundsForRebuild(void)
{
	int i;

	for (i = 0; i < NumPlayers; i++)
	{
		PLAYER* p = &player[i];
		int carId = p->playerCarId;

		if (p->playerType != PLAYER_TYPE_CAR || carId < 0 || carId >= MAX_CARS)
			continue;

		StartPlayerCarSounds(i, car_data[carId].ap.model, (VECTOR*)car_data[carId].hd.where.t);
	}
}

/* A bank rebuild can move every appended block, so an AI siren entry that
 * survives it would point at whatever now occupies the old SPU address. Free
 * them all; DoDopplerSFX() rebuilds the ones whose car still qualifies. */
static void ClearSirenNoises(void)
{
	int i;

	for (i = 0; i < MAX_SIREN_NOISES; i++)
	{
		if (siren_noise[i].stopped == 0 && siren_noise[i].chan >= 0)
		{
			StopChannel(siren_noise[i].chan);
			UnlockChannel(siren_noise[i].chan);
		}

		siren_noise[i].chan = -1;
		siren_noise[i].car = MAX_CARS;
		siren_noise[i].in_use = 0;
		siren_noise[i].stopped = 1;
		siren_noise_sample[i] = 0;
	}
}

/* JERICHO: reclaim the blocks imported slots appended after the level's own
 * banks. A level can change cars indefinitely, and appending a fresh 3-sample
 * bank on every change eventually fills SOUND_BANK_CARS; that made the slot
 * fall back to the stock slot-index formula and play another vehicle's engine.
 *
 * Rewinding to the checkpoint the level saved (after its own and special
 * banks) drops every appended block, then the live slots are loaded again from
 * their CURRENT resident model/source. The result is one block per live slot,
 * which the capacity invariant in sound.h guarantees fits. */
static void RebuildImportedCarSoundBanks(void)
{
	int reload[MAX_CAR_RESIDENT_MODELS];
	int n = 0;
	int i, slot;

	if (!car_sound_checkpoint_ready || car_sound_rebuilding)
		return;

	/* Snapshot before clearing: re-loading by slot also covers any spare slot
	 * a hot load touched, not just the range LoadExtraCarSoundBanks walks. */
	for (slot = 0; slot < MAX_CAR_RESIDENT_MODELS; slot++)
	{
		if (car_slot_appended[slot])
			reload[n++] = slot;
	}

	car_sound_rebuilding = 1;

	/* Back to the level's own and special banks, then drop the mappings that
	 * pointed into the appended region. */
	LoadSoundBankDynamic(NULL, 2, SOUND_BANK_CARS);	// addr = memtop
	LoadSoundBankDynamic(NULL, 4, SOUND_BANK_CARS);	// count = banktop

	for (i = 0; i < n; i++)
	{
		slot = reload[i];

		car_slot_sample[slot] = -1;
		car_slot_city[slot] = -1;
		car_slot_model[slot] = -1;
		car_slot_imported[slot] = 0;
		car_slot_appended[slot] = 0;
	}

	for (i = 0; i < n; i++)
		LoadCarSoundForSlot(reload[i]);

	RestartPlayerCarSoundsForRebuild();
	ClearSirenNoises();

	car_sound_rebuilding = 0;

	printInfo("gamesnd: rebuilt %d imported car sound block(s) - bank now holds %d sample(s)\n",
		n, GetSoundBankSampleCount(SOUND_BANK_CARS));
}

/* Give `slot` a car sound bank of its own and record the SOUND_BANK_CARS sample
 * block it landed in. Unlike the stock slots, an imported slot's bank is
 * appended AFTER the level's own and special banks, so GetCarBankSample cannot
 * derive its block from the slot index.
 *
 * Idempotent: the same (city, model) already mapped for this slot is a no-op.
 * A released/reused slot therefore notices it needs a different bank. */
void LoadCarSoundForSlot(int slot)
{
	int city, model, before, after;
	int old_sample, old_city, old_model, old_imported, old_appended;
	int rewound = 0;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS || slot == SPECIAL_CAR_SLOT)
		return;

	if (!car_sound_ready)
		return;

	// The slot's CURRENT car, before any reclaim work: an unchanged mapping is
	// a no-op.
	model = residentCarModels[slot];

	if (model < 0 || model >= CAR_MODEL_LUMP_ENTRIES)
	{
		car_slot_sample[slot] = -1;
		car_slot_imported[slot] = 0;
		car_slot_appended[slot] = 0;
		return;
	}

	city = GetCarModelSourceCity(slot);

	if (city < 0)
		city = GameLevel;

	if (car_slot_sample[slot] >= 0 &&
		car_slot_city[slot] == city && car_slot_model[slot] == model)
		return;

	// A car's samples are addressed as block*3, so the appended bank must start
	// on a 3-sample boundary. The level's special banks need not end on one.
	// Align BEFORE the room check: the padding is part of what has to fit.
	AlignSoundBankSamples(SOUND_BANK_CARS, 3);

	// Every imported car bank is three samples. If one more would fill the
	// bank, reclaim the appended blocks first. One block per live slot fits,
	// so this is what keeps a long session switching correctly.
	if (car_sound_checkpoint_ready && !car_sound_rebuilding &&
		!SoundBankHasRoom(SOUND_BANK_CARS, 3))
	{
		RebuildImportedCarSoundBanks();
		rewound = 1;

		// The rebuild reloads the live slots itself; if this one was among
		// them its new mapping is already here.
		if (car_slot_sample[slot] >= 0 &&
			car_slot_city[slot] == city && car_slot_model[slot] == model)
			return;
	}

	/* What the slot had before this attempt. A failed load keeps it only when
	 * no rebuild has rewound the bank underneath it (a rewound block would be
	 * stale, and the stock formula is the safe fallback there). */
	old_sample = car_slot_sample[slot];
	old_city = car_slot_city[slot];
	old_model = car_slot_model[slot];
	old_imported = car_slot_imported[slot];
	old_appended = car_slot_appended[slot];

	// A Driver 1 city's synthetic LEV carries its own engine/idle bank.
	if (LoadEmbeddedCarSoundForSlot(slot))
	{
		car_slot_appended[slot] = 1;
		RestartSirenForSlot(slot);
		return;
	}

	before = GetSoundBankSampleCount(SOUND_BANK_CARS);

	// drop the old mapping first: if the load fails the slot must fall back to
	// the stock formula rather than keep pointing at its previous car's samples
	car_slot_sample[slot] = -1;
	car_slot_imported[slot] = 0;
	car_slot_appended[slot] = 0;

	LoadBankFromLump(SOUND_BANK_CARS, MapCarIndexToBank(slot));

	after = GetSoundBankSampleCount(SOUND_BANK_CARS);

	if (after <= before)
	{
		// One reclaim-and-retry. Marking the slot as appended makes the rebuild
		// reload it too, so the retry is the rebuild's own LoadCarSoundForSlot.
		if (!rewound && !car_sound_rebuilding && car_sound_checkpoint_ready)
		{
			car_slot_appended[slot] = 1;
			RebuildImportedCarSoundBanks();

			if (car_slot_sample[slot] >= 0 &&
				car_slot_city[slot] == city && car_slot_model[slot] == model)
				return;

			// The bank has been rewound: the old block is no longer valid.
			rewound = 1;
		}

		if (!rewound && !car_sound_rebuilding && old_sample >= 0)
		{
			car_slot_sample[slot] = old_sample;
			car_slot_city[slot] = old_city;
			car_slot_model[slot] = old_model;
			car_slot_imported[slot] = old_imported;
			car_slot_appended[slot] = old_appended;

			printWarning("gamesnd: no car sound bank for resident slot %d (%s model %d) - keeping block %d\n",
				slot, LevelNames[city], model, old_sample);
			return;
		}

		printWarning("gamesnd: no car sound bank for resident slot %d (%s model %d)\n",
			slot, LevelNames[city], model);
		return;
	}

	car_slot_sample[slot] = before / 3;
	car_slot_city[slot] = city;
	car_slot_model[slot] = model;
	car_slot_imported[slot] = 0;
	car_slot_appended[slot] = 1;

	printInfo("gamesnd: resident slot %d (%s model %d) -> car sound block %d\n",
		slot, LevelNames[city], model, car_slot_sample[slot]);

	RestartCarNoiseForSlot(slot);
	RestartSirenForSlot(slot);
}

#if MAX_CAR_RESIDENT_MODELS > 5
/* Append banks for the slots LoadLevelSFX does not stock-load: slot 3 (when it
 * is not sharing slot 2's bank) and every spare slot. This runs before
 * LoadGameLevel builds the geometry, so it keys off the resident models the
 * import set wrote; a car that later fails to build keeps this bank, which is
 * the least surprising outcome (its slot still names that model). */
static void LoadExtraCarSoundBanks(void)
{
	int slot;

	for (slot = 3; slot < SPECIAL_CAR_SLOT; slot++)
	{
		if (slot == 4)
			continue;			// stock-loaded beside slots 0..2

		if (residentCarModels[slot] < 0 || residentCarModels[slot] >= CAR_MODEL_LUMP_ENTRIES)
			continue;

		if (slot == 3 && GetCarModelSourceCity(3) < 0)
			continue;			// stock cop car keeps sharing slot 2's bank

		LoadCarSoundForSlot(slot);
	}
}
#endif

static char cop_model = 0;
int gDoCopSpeech = 1;		// [A]

// [D] [T]
void LoadLevelSFX(int missionNum)
{
	int index;
	int i;
	u_int city_night_fx;

	city_night_fx = (gTimeOfDay == TIME_NIGHT);

	// A new level reloads every car bank. Drop the per-slot blocks an earlier
	// level's imports recorded so GetCarBankSample cannot point at a block that
	// this level has not loaded.
	car_sound_ready = 0;
	car_sound_checkpoint_ready = 0;
	car_sound_rebuilding = 0;
	car_zero_siren_reported = 0;
	imported_siren_reported = 0;
	civilian_import_reported = 0;
	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
	{
		car_slot_sample[i] = -1;
		car_slot_city[i] = -1;
		car_slot_model[i] = -1;
		car_slot_imported[i] = 0;
		car_slot_appended[i] = 0;
	}

	car_sound_ready = 1;

	cop_bank = missionNum % 4 + 1;
	cop_model = 3;

	// init sound bank memory
	LoadSoundBankDynamic(NULL, 0, 0);

	// load car banks. An embedded (Driver 1 bake) bank wins over the D2 host
	// lookup; without one the stock path is exactly as before.
	for (i = 0; i < 3; i++)
	{
		if (!LoadEmbeddedCarSoundForSlot(i))
			LoadBankFromLump(SOUND_BANK_CARS, MapCarIndexToBank(i));
	}
#if MAX_CAR_RESIDENT_MODELS > 5
	if (!LoadEmbeddedCarSoundForSlot(4))
		LoadBankFromLump(SOUND_BANK_CARS, MapCarIndexToBank(4));
#endif

	ShowLoading();

	// load footsteps, car effects etc
	//LoadBankFromLump(SOUND_BANK_SFX, SBK_ID_MENU);
	LoadBankFromLump(SOUND_BANK_SFX, SBK_ID_SFX);
	LoadBankFromLump(SOUND_BANK_TANNER, SBK_ID_TANNER );

	if (GameLevel & 2)
		LoadBankFromLump(SOUND_BANK_VOICES, SBK_COP_SIREN_START + (GameLevel & 1) * 2);
	else
		LoadBankFromLump(SOUND_BANK_VOICES, SBK_COP_SIREN_START + (GameLevel & 3));

	// Load cop voices except those missions
	if (missionNum - 1U > 3 && missionNum != 6 && missionNum != 7 &&
		missionNum != 9 && missionNum != 10 && missionNum != 11 &&
		missionNum != 13 && missionNum != 14 && missionNum != 18 &&
		missionNum != 19 && missionNum != 20 && missionNum != 22 &&
		missionNum != 26 && missionNum != 28 && missionNum != 31 &&
		missionNum != 33 && missionNum != 34 && missionNum != 38 &&
		missionNum != 40)
	{
		// first bank - directions
		// second bank - 
		if (GameLevel & 2)
		{
			LoadBankFromLump(SOUND_BANK_VOICES, SBK_COP_PHRASES_START + (GameLevel & 1) * 8 + (GameLevel & 1) * 2);
			LoadBankFromLump(SOUND_BANK_VOICES, SBK_COP_PHRASES_START + (GameLevel & 1) * 10 + cop_bank);
		}
		else
		{
			LoadBankFromLump(SOUND_BANK_VOICES, SBK_COP_PHRASES_START + (GameLevel & 3) * 4 + (GameLevel & 3));
			LoadBankFromLump(SOUND_BANK_VOICES, SBK_COP_PHRASES_START + (GameLevel & 3) * 5 + cop_bank);
		}
	}

	ShowLoading();

	// load ambient effects
	if (NumPlayers < 2 || NoPlayerControl != 0)
	{
		if (GameLevel == 0)
			LoadBankFromLump(SOUND_BANK_ENVIRONMENT, SBK_CITY_EFFECTS_START + city_night_fx);
		else if (GameLevel == 1)
			LoadBankFromLump(SOUND_BANK_ENVIRONMENT, SBK_CITY_EFFECTS_START + city_night_fx + 2);
		else if (GameLevel == 2)
			LoadBankFromLump(SOUND_BANK_ENVIRONMENT, SBK_CITY_EFFECTS_START + city_night_fx + 4);
		else if (GameLevel == 3)
			LoadBankFromLump(SOUND_BANK_ENVIRONMENT, SBK_CITY_EFFECTS_START + city_night_fx + 6);
	}

	// total phrases
	phrase_top = 0;

	if (missionNum - 2U < 3 || missionNum == 9 || missionNum == 10 || missionNum == 27)
	{
		LoadBankFromLump(SOUND_BANK_MISSION, SBK_ID_JONES);
		phrase_top = 7;
	}
	else if (missionNum - 20U < 2 || missionNum == 25 || missionNum == 39)
	{
		LoadBankFromLump(SOUND_BANK_MISSION, SBK_ID_JERICHO);
		phrase_top = 3;
	}

	switch (missionNum)
	{
		case 2:
			index = SBK_ID_MISSION_2;
			break;
		case 3:
			index = SBK_ID_MISSION_3;
			break;
		case 4:
			index = SBK_ID_MISSION_4;
			break;
		case 10:
			index = SBK_ID_MISSION_10;
			break;
		case 13:
			index = SBK_ID_MISSION_13;
			break;
		case 15:
			index = SBK_ID_FERRY;
			break;
		case 16:
			index = SBK_ID_FERRY;
			break;
		case 18:
			index = SBK_ID_MISSION_18;
			break;
		case 22:
			index = SBK_ID_MISSION_22;
			break;
		case 23:
			index = SBK_ID_MISSION_23;
			break;
		case 24:
			index = SBK_ID_MISSION_24;
			break;
		case 27:
			index = SBK_ID_MISSION_27;
			break;
		case 29:
			index = SBK_ID_MISSION_29;
			break;
		case 30:
			index = SBK_ID_MISSION_30;
			break;
		case 25:
		case 32:
			index = SBK_ID_MISSION_32;
			break;
		case 33:
			index = SBK_ID_MISSION_33;
			break;
		case 35:
			index = SBK_ID_MISSION_35;
			break;
		case 39:
			index = SBK_ID_MISSION_39;
			break;
		case 40:
			index = SBK_ID_MISSION_40;
			break;
		case 52:
		case 53:
			index = SBK_ID_HAVANA_TAKEADRIVE;	// [A] load Havana bank again
			break;
		case 54:
		case 55:
			index = SBK_ID_VEGAS_TAKEADRIVE;
			break;
		case 11:
		case 20:
		case 21:
		case 56:
		case 57:
			index = SBK_ID_MISSION_11;
			break;
		default:
			index = 0;
	}

	// mission bank	
	if (index != 0)
		LoadBankFromLump(SOUND_BANK_MISSION, index);

	// special siren bank
	if (GameLevel == 0 || GameLevel == 3)
		LoadBankFromLump(SOUND_BANK_SFX, SBK_ID_SPECIAL_SIREN1);
	else if (GameLevel == 2)
		LoadBankFromLump(SOUND_BANK_SFX, SBK_ID_SPECIAL_SIREN2);

	// [A] padding?
	LoadSoundBankDynamic(NULL, 1, SOUND_BANK_DUMMY);
	LoadSoundBankDynamic(NULL, 3, SOUND_BANK_CARS);

	// special vehicle 1 bank
	if (missionNum == 40 || missionNum >= 400 && missionNum <= 404)
		LoadBankFromLump(SOUND_BANK_CARS, MapCarIndexToBank(SPECIAL_CAR_SLOT));
	else
		LoadBankFromLump(SOUND_BANK_CARS, SpecialVehicleKludge(0));

	// special vehicle 2 bank
	if (missionNum != 24 && missionNum != 27 &&
		missionNum != 29 && missionNum != 30 &&
		missionNum != 35)
	{
		LoadBankFromLump(SOUND_BANK_CARS, SpecialVehicleKludge(1));
	}

	// secret car sound bank
	if (missionNum - 50U < 16 || missionNum >= 400)
	{
		LoadBankFromLump(SOUND_BANK_CARS, SpecialVehicleKludge(2));
	}

	// disable cop speech on specific missions (gangs)
	// and set cop model (car sound bank)
	if (missionNum == 7 || missionNum == 9 ||
		missionNum == 11 || missionNum == 20 ||
		missionNum == 26 || missionNum == 31 ||
		missionNum == 33 || missionNum == 40)
	{
		gDoCopSpeech = 0;

		for (i = 0; i < 3; i++)
		{
			if (residentCarModels[i] == residentCarModels[3])
				cop_model = i;
		}
	}
	else
	{
		gDoCopSpeech = 1;
	}

	// JERICHO: the bank state a level's own cars and special vehicles leave
	// behind. Imported slots append after this point, and a level can change
	// cars many times; when the appended blocks would fill the bank, this is
	// where they are rewound to so only ONE block per live slot is held.
	// Nothing else consumes memtop/banktop, so overwriting the earlier save
	// (which sits before the special banks) is safe.
	LoadSoundBankDynamic(NULL, 1, SOUND_BANK_CARS);	// save memtop
	LoadSoundBankDynamic(NULL, 3, SOUND_BANK_CARS);	// save banktop

#if MAX_CAR_RESIDENT_MODELS > 5
	/* One 3-sample block must fit for every possible appended slot, on top of
	 * the 7 blocks the level itself holds (slots 0,1,2,4 plus up to three
	 * special banks). At 16 slots that is 18 blocks / 54 samples, inside the
	 * 64-sample bank. A future slot raise that breaks this would make a
	 * rebuild unable to represent the live set, so say so at level load. */
	{
		int needed = (7 + (MAX_CAR_RESIDENT_MODELS - 5)) * 3;

		if (needed > GetSoundBankCapacity())
			printWarning("gamesnd: SOUND_BANK_CARS holds %d samples but %d resident slots need %d - imported car sounds cannot all fit\n",
				GetSoundBankCapacity(), MAX_CAR_RESIDENT_MODELS, needed);
	}

	// Imported slots' banks are appended after the level's own and special
	// banks, so their stock block arithmetic above is left exactly as it was.
	LoadExtraCarSoundBanks();
#endif

	// From here on an append can be reclaimed against the checkpoint above.
	car_sound_checkpoint_ready = 1;
}

int TimeSinceLastSpeech = 0;

// [A] start car sounds for player
void StartPlayerCarSounds(int playerId, int model, VECTOR* pos)
{
	int carSampleId;
	int channel;
	int siren;

	carSampleId = GetCarBankSample(model);
	siren = CarHasSiren(model);

	// rev sound
	channel = playerId * 3;	
	Start3DSoundVolPitch(channel, SOUND_BANK_CARS, carSampleId * 3, pos->vx, pos->vy, pos->vz, -10000, 4096);

	// idle sound
	channel = playerId * 3 + 1;	
	Start3DSoundVolPitch(channel, SOUND_BANK_CARS, carSampleId * 3 + 1, pos->vx, pos->vy, pos->vz, -10000, 4096);

	channel = playerId * 3 + 2;

	if (siren)
	{
		// SOUND_BANK_SFX or SOUND_BANK_VOICES
		Start3DSoundVolPitch(channel, (siren & 0xff00) >> 8, siren & 0xff, pos->vx, pos->vy, pos->vz, -10000, 129);
		SoundLogCarSample("siren", model, siren & 0xff);
	}
	else
	{
		/* This is the player's siren loop (and the same index carries the
		 * horn one-shot). A car with no siren must not leave the previous
		 * car's siren playing after a change. */
		StopChannel(channel);
	}

	if (NumPlayers > 1 && NoPlayerControl == 0)
	{
		channel = playerId * 3;
		SetPlayerOwnsChannel(channel, playerId);

		channel = playerId * 3 + 1;
		SetPlayerOwnsChannel(channel, playerId);

		if (siren)
		{
			channel = playerId * 3 + 2;
			SetPlayerOwnsChannel(channel, playerId);
		}
	}
}

// [D] [T]
void StartGameSounds(void)
{
	int i;
	PLAYER* lcp;
	CAR_DATA* cp;
	int car_model;

	TimeSinceLastSpeech = 0;

	lcp = player;
	for (i = 0; i < NumPlayers; i++)
	{
		if (lcp->playerType == PLAYER_TYPE_CAR)
		{
			cp = &car_data[lcp->playerCarId];
			StartPlayerCarSounds(i, cp->ap.model, (VECTOR*)cp->hd.where.t);
		}

		lcp->crash_timer = 0;

		lcp->revsvol = -10000;
		lcp->idlevol = -8000;
		lcp++;
	}

	if (NumPlayers == 1)
	{
		SpuSetVoiceAR(2, 27);
		SpuSetVoiceAR(5, 27);
	}

	InitEnvSnd(MAX_LEVEL_ENVSOUNDS);
	
	if (NumPlayers < 2 || NoPlayerControl != 0)
		AddEnvSounds(GameLevel, gTimeOfDay);

	InitDopplerSFX();
	InitSkidding();

	AddTunnels(GameLevel);

	first_offence = 1;

	InitializeMissionSound();
}

// [D] [T]
ushort GetEngineRevs(CAR_DATA* cp)
{
	int acc;
	int gear;
	int ws;
	int type;
	int g;

	gear = cp->hd.gear;
	ws = cp->hd.wheel_speed;
	acc = cp->thrust;
	type = (cp->controlType == CONTROL_TYPE_CIV_AI);

	if (ws > 0)
	{
		ws >>= 11;

		if (gear > 3)
			gear = 3;

		// JERICHO-HOOK: retune the gear/rev model per car (cainescrossfire's short
		// gears + tall top gear that levels pitch at the car's top speed).
		// Prefilled with the stock row, so no handler = exactly stock.
		{
			JER_ARGS_CAR_GEARBOX gb;
			int gi;

			gb.car = cp;
			gb.type = type;
			gb.wheelSpeed = ws;
			gb.thrust = acc;
			gb.revCeiling = 0;

			for (gi = 0; gi < 4; gi++)
			{
				gb.lowIdleWs[gi] = geard[type][gi].lowidl_ws;
				gb.lowWs[gi] = geard[type][gi].low_ws;
				gb.hiWs[gi] = geard[type][gi].hi_ws;
				gb.ratioAc[gi] = geard[type][gi].ratio_ac;
				gb.ratioIdle[gi] = geard[type][gi].ratio_id;
			}

			jer_fire(JER_EVENT_CAR_GEARBOX, &gb);

			// walk gears with the (possibly retuned) table. Matches the stock
			// algorithm exactly: a downshift is only accepted when the car is
			// also slow enough for the lower gear (its hiWs is re-checked in
			// the SAME iteration, so the walk can never oscillate between two
			// gears when gear bands overlap, e.g. the CIV row). A hard
			// iteration guard keeps a hostile table from hanging the game.
			g = gear;
			if (g < 0) g = 0;
			if (g > 3) g = 3;

			for (gi = 0; gi < 8; gi++)
			{
				int ng = g;
				int lws = (acc < 1) ? gb.lowIdleWs[g] : gb.lowWs[g];

				if (ws < lws && g > 0)
				{
					ng = g - 1;               // downshift candidate

					if (gb.hiWs[ng] < ws)     // too fast for the lower gear
						ng = g;               // stay in the current gear
				}
				else if (gb.hiWs[g] < ws)
					ng = g + 1;               // upshift

				if (ng == g)
					break;

				if (ng < 0) { g = 0; break; }
				if (ng > 3) { g = 3; break; }

				g = ng;
			}

			cp->hd.gear = g;

			if (acc != 0)
			{
				int revs = ws * gb.ratioAc[g];
				if (gb.revCeiling > 0 && revs > gb.revCeiling)
					revs = gb.revCeiling;
				return (ushort)revs;
			}

			{
				int revs = ws * gb.ratioIdle[g];
				if (gb.revCeiling > 0 && revs > gb.revCeiling)
					revs = gb.revCeiling;
				return (ushort)revs;
			}
		}
	}
	else
	{
		ws = -ws / 2048;

		cp->hd.gear = 0;

		// reverse redline: reverse drives as fast as forward in cainescrossfire, so
		// the raw ws * ratio_ac can blow past redline (and overflow ushort)
		if (acc != 0)
		{
			int revs = ws * geard[type][0].ratio_ac;
			if (revs > 23000) revs = 23000;
			return (ushort)revs;
		}

		{
			int revs = ws * geard[type][0].ratio_id;
			if (revs > 23000) revs = 23000;
			return (ushort)revs;
		}
	}
}

const int maxrevdrop = 1440;
const int maxrevrise = 1600;

// [D] [T]
void ControlCarRevs(CAR_DATA* cp)
{
	char spin;
	int player_id, acc, oldvol;
	int revRise;
	int revDrop;
	short oldRevs, newRevs, desiredRevs;

	acc = cp->thrust;
	spin = cp->wheelspin;
	oldRevs = cp->hd.revs;
	player_id = GetPlayerId(cp);

	// JERICHO-HOOK: module-controlled rev slew. A module scales how fast the
	// engine pitch climbs (revRise) and falls (revDrop) toward its target;
	// defaults are the stock limits, so no handler = exactly stock.
	{
		JER_ARGS_CAR_REVS revArgs;

		revArgs.car = cp;
		revArgs.revRise = maxrevrise;
		revArgs.revDrop = maxrevdrop;

		jer_fire(JER_EVENT_CAR_REVS, &revArgs);

		revRise = revArgs.revRise;
		revDrop = revArgs.revDrop;
	}

	cp->hd.changingGear = 0;

	if (spin == 0 && (cp->hd.wheel[1].susCompression || cp->hd.wheel[3].susCompression || acc == 0))
	{
		desiredRevs = GetEngineRevs(cp);
	}
	else
	{
		desiredRevs = 20160;

		if (cp->hd.wheel[1].susCompression == 0 && cp->hd.wheel[3].susCompression == 0)
		{
			desiredRevs = 30719;
			spin = 1;
		}

		if (oldRevs < 8000)
			oldRevs = 8000;

		cp->hd.gear = 0;
	}

	newRevs = desiredRevs;
	desiredRevs = (oldRevs - newRevs);

	if (revDrop < desiredRevs)
	{
		acc = 0;
		cp->hd.changingGear = 1;
		newRevs = oldRevs - revDrop;
	}

	desiredRevs = newRevs - oldRevs;

	if (revRise < desiredRevs)
		newRevs = oldRevs + revRise;

	cp->hd.revs = newRevs;
	if (player_id != -1)
	{
		if (acc == 0 && newRevs < 7001)
		{
			acc = player[player_id].revsvol;

			player[player_id].idlevol += 200;
			player[player_id].revsvol = acc - 200;

			if (player[player_id].idlevol > -6000)
				player[player_id].idlevol = -6000;

			if (player[player_id].revsvol < -10000)
				player[player_id].revsvol = -10000;
		}
		else
		{
			int revsmax;

			if (acc != 0)
				revsmax = -5500;
			else
				revsmax = -6750;

			if (spin == 0)
				acc = -64;
			else
				acc = -256;

			player[player_id].idlevol += acc;

			if (spin == 0)
				acc = 175;
			else
				acc = 700;

			player[player_id].revsvol = player[player_id].revsvol + acc;

			if (player[player_id].idlevol < -10000)
				player[player_id].idlevol = -10000;

			if (player[player_id].revsvol > revsmax)
				player[player_id].revsvol = revsmax;
		}
	}
}


// [D] [T]
void DoSpeech(int chan, int sound)
{
	if (sound >= 100)
		StartSound(chan, SOUND_BANK_MISSION, sound - 100, 0, 4096);
	else if (sound != 0)
		StartSound(chan, SOUND_BANK_VOICES, sound, -1500, 4096);
}

// [D] [T]
char PlaySpeech(SPEECH_QUEUE* pSpeechQueue, int sound)
{
	int i;

	if (pSpeechQueue->count >= 7)
		return 0;

	i = pSpeechQueue->count - 1;

	while (i >= 0)
	{
		pSpeechQueue->slot[i + 1] = pSpeechQueue->slot[i];
		i--;
	}

	pSpeechQueue->slot[0] = sound;
	pSpeechQueue->count++;

	return 1;
}

// [D] [T]
void InitSpeechQueue(SPEECH_QUEUE* pSpeechQueue)
{
	ClearMem((char*)pSpeechQueue, sizeof(SPEECH_QUEUE));

	if (GameType != GAME_MISSION)
		pSpeechQueue->allowed = 2;
}

// [D] [T]
void ControlSpeech(SPEECH_QUEUE* pSpeechQueue)
{
	if (GameType == GAME_MISSION)
	{
		if (bMissionTitleFade == 0)
		{
			if (pSpeechQueue->allowed == 1)
				pSpeechQueue->allowed = 2;
		}
		else
			pSpeechQueue->allowed = 1;

		if (pSpeechQueue->allowed != 2)
			return;
	}

	if (pSpeechQueue->count < 1)
	{
		TimeSinceLastSpeech++;

		if (pSpeechQueue->is_playing != 0 && SpuGetKeyStatus(SPU_VOICECH(pSpeechQueue->chan)) == 0)
		{
			SetReverbChannelState(pSpeechQueue->chan, pSpeechQueue->reverb);
			UnlockChannel(pSpeechQueue->chan);

			pSpeechQueue->is_playing = 0;
		}
	}
	else if (pSpeechQueue->is_playing == 0)
	{
		pSpeechQueue->chan = GetFreeChannel();

		LockChannel(pSpeechQueue->chan);

		pSpeechQueue->reverb = SetReverbChannelState(pSpeechQueue->chan, 0);
		pSpeechQueue->is_playing = 1;

		channels[pSpeechQueue->chan].time = 0;

		pSpeechQueue->count--;

		DoSpeech(pSpeechQueue->chan, pSpeechQueue->slot[pSpeechQueue->count]);
		TimeSinceLastSpeech = 0;
	}
	else if (SpuGetKeyStatus(SPU_VOICECH(pSpeechQueue->chan)) == 0)
	{
		pSpeechQueue->count--;

		DoSpeech(pSpeechQueue->chan, pSpeechQueue->slot[pSpeechQueue->count]);
		TimeSinceLastSpeech = 0;
	}
}

// [D] [T]
void CopSay(int phrase, int direction)
{
	if (!gDoCopSpeech)
		return;

	PlaySpeech(&gSpeechQueue, phrase);

	if (phrase == 10)
	{
		if (cop_bank == 1)
			return;

		if (cop_bank != 2 && cop_bank != 3)
			return;

	}
	else
	{
		if (phrase != 15)
			return;

		if (cop_bank != 1) 
		{
			if (cop_bank == 2)
				return;
			
			if (cop_bank == 3)
				return;

			if (cop_bank != 4)
				return;
		}
	}

	PlaySpeech(&gSpeechQueue, direction + 1);
}


// [D] [T]
void BodSay(int phrase)
{
	if (phrase_top != 0 && phrase < phrase_top)
		PlaySpeech(&gSpeechQueue, phrase + 100);
}

// [D] [T]
void MissionSay(int phrase)
{
	phrase = GetMissionSound(phrase);

	if (phrase != -1)
		PlaySpeech(&gSpeechQueue, phrase + 100);
}

// [D] [T]
void InitDopplerSFX(void)
{
	int i;

	for (i = 0; i < MAX_SIREN_NOISES; i++)
	{
		siren_noise[i].chan = -1;
		siren_noise[i].car = MAX_CARS;
		siren_noise[i].in_use = 0;
		siren_noise[i].stopped = 1;
		siren_noise_sample[i] = 0;
	}

	for (i = 0; i < MAX_CAR_NOISES; i++)
	{
		car_noise[i].chan = -1;
		car_noise[i].car = MAX_CARS;
		car_noise[i].in_use = 0;
		car_noise[i].stopped = 1;
	}

	if (GameType == GAME_GETAWAY)
		loudhail_time = 245;
	else
		loudhail_time = 75;
}

char force_idle[8] = { 0 };
char force_siren[8] = { 0 };

// [D] [T]
void DoDopplerSFX(void)
{
	int pitch, volume, sample;
	short* playerFelony;
	int i, j;
	int car;
	u_int car_flags;
	int num_noisy_cars;
	int sirens;

	u_int car_dist[MAX_CARS];
	ushort indexlist[MAX_CARS];

	CAR_DATA* car_ptr;
	int dx, dz;
	u_int dist;

	num_noisy_cars = 0;

	// collect sounding cars and estimate their distance to camera
	for (i = 0; i < MAX_CARS; i++)
	{
		car_ptr = &car_data[i];

		dx = car_ptr->hd.where.t[0] - camera_position.vx;
		dz = car_ptr->hd.where.t[2] - camera_position.vz;

		if (ABS(dx) < 16384 && ABS(dz) < 16384)
		{
			if (car_ptr->controlType == CONTROL_TYPE_CIV_AI && car_ptr->ai.c.ctrlState != CIV_AI_CTRL_PARKED && car_ptr->ai.c.ctrlState != CIV_AI_CTRL_EMPTY)
			{
				dist = jsqrt(dx * dx + dz * dz) + 0x6000;
			}
			else if (car_ptr->controlType == CONTROL_TYPE_PURSUER_AI ||
				car_ptr->controlType == CONTROL_TYPE_LEAD_AI ||
				car_ptr->controlType == CONTROL_TYPE_CUTSCENE && SilenceThisCar(i) == 0)
			{
				dist = jsqrt(dx * dx + dz * dz);
			}
			else
				continue;

			indexlist[num_noisy_cars] = i;
			car_dist[i] = dist;
			num_noisy_cars++;
		}
	}

	// sort cars by distance
	for (i = 1; i < num_noisy_cars; i++)
	{
		int tmpi;
		tmpi = indexlist[i];

		j = i - 1;
		while (j >= 0 && car_dist[indexlist[j]] > car_dist[tmpi])
		{
			indexlist[j + 1] = indexlist[j];
			j = j - 1;
		}
		indexlist[j + 1] = tmpi;
	}

	car_flags = 0;
	sirens = 0;

	// collect cop cars for siren sound
	for (i = 0; i < num_noisy_cars && sirens < 2; i++)
	{
		int siren;
		car_ptr = &car_data[indexlist[i]];

		siren = 0;

		if (handlingType[car_ptr->hndType].fourWheelDrive == 1 &&
			car_ptr->controlType == CONTROL_TYPE_PURSUER_AI && car_ptr->ai.p.dying < 75 &&
			CarHasSiren(car_ptr->ap.model) != 0)
		{
			siren = 1;
		}

		// sound up ambulance we're going to steal
		if (gCurrentMissionNumber == 26)
		{
			if (car_ptr->ap.model == SPECIAL_CAR_SLOT && car_ptr->controlType == CONTROL_TYPE_CUTSCENE)
			{
				siren = 1;
			}
		}

		// any cutscene cop car or car with forced siren
		if (gInGameCutsceneActive != 0 && car_ptr->controlType == CONTROL_TYPE_CUTSCENE && force_siren[indexlist[i]] != 0)
		{
			siren = 1;
		}

		// play car music
		// vans in 'Caine's Compound' should not listen to it
		if (gCurrentMissionNumber != 7 && 
			car_ptr->controlType == CONTROL_TYPE_CIV_AI && 
			car_ptr->ap.model > 0 && car_ptr->ap.model < (MAX_CAR_RESIDENT_MODELS-2) &&
			indexlist[i] == 1)
		{
			siren = 1;
		}

		if (!siren)
			continue;

		car_flags |= 1 << indexlist[i];

		if (gInGameCutsceneActive == 0)
			sirens++;
	}

	/* A live siren entry whose packed sample changed (the slot was re-banked, or
	 * the car became/stopped being a cop) must be re-issued. Free the entry but
	 * leave the car's bit in car_flags, so the start loop below starts it again
	 * with the new sample instead of keeping the old SPU data. */
	for (i = 0; i < MAX_SIREN_NOISES; i++)
	{
		int car = siren_noise[i].car;
		int current;

		if (siren_noise[i].stopped != 0 || car < 0 || car >= MAX_CARS)
			continue;

		current = (car_data[car].controlType != CONTROL_TYPE_CIV_AI)
			? CarHasSiren(car_data[car].ap.model) : 0;

		if (current == siren_noise_sample[i])
			continue;

		StopChannel(siren_noise[i].chan);
		UnlockChannel(siren_noise[i].chan);

		siren_noise[i].chan = -1;
		siren_noise[i].car = MAX_CARS;
		siren_noise[i].in_use = 0;
		siren_noise[i].stopped = 1;
		siren_noise_sample[i] = 0;
	}

	// stop unused siren noises
	for (i = 0; i < MAX_SIREN_NOISES; i++)
	{
		int in_use, music;
		
		in_use = (car_flags & 1 << siren_noise[i].car) != 0;
		music = siren_noise[i].idle;

		siren_noise[i].in_use = in_use;
		car_flags &= ~(in_use << siren_noise[i].car);

		if ((in_use == 0 || music != (car_data[siren_noise[i].car].controlType == CONTROL_TYPE_CIV_AI)) && siren_noise[i].stopped == 0)
		{
			StopChannel(siren_noise[i].chan);
			UnlockChannel(siren_noise[i].chan);

			siren_noise[i].chan = -1;
			siren_noise[i].car = MAX_CARS;
			siren_noise[i].stopped = 1;
			siren_noise_sample[i] = 0;
		}
	}

	// start sirens
	for (i = 0; i < num_noisy_cars; i++)
	{
		if (car_flags & 1 << indexlist[i])
		{
			car = indexlist[i];

			// dispatch siren sounds
			for (j = 0; j < MAX_SIREN_NOISES; j++)
			{
				int siren;
				int sample;

				if (siren_noise[j].in_use != 0)
					continue;

				siren = car_data[car].controlType != CONTROL_TYPE_CIV_AI;

				if (siren)
				{
					sample = CarHasSiren(car_data[car].ap.model);

					/* A forced-siren cutscene car can reach this loop without a
					 * siren sample (CarHasSiren 0). Starting bank 0 / sample 0
					 * would play an unrelated sound; leave the car silent and
					 * say so once per level. */
					if (sample == 0)
					{
						if (!(car_zero_siren_reported & (1u << car)))
						{
							car_zero_siren_reported |= 1u << car;
							printInfo("gamesnd: car %d (slot %d) is flagged for a siren but has no siren sample - skipping\n",
								car, car_data[car].ap.model);
						}

						break;
					}
				}
				else
					sample = 0;

				siren_noise[j].in_use = 1;
				siren_noise[j].stopped = 0;
				siren_noise[j].car = car;
				siren_noise[j].idle = siren == 0;
				siren_noise_sample[j] = sample;

				if (siren)
				{
					siren_noise[j].chan = Start3DTrackingSound(-1, (sample & 0xff00) >> 8, sample & 0xff,
						(VECTOR*)car_data[car].hd.where.t,
						(LONGVECTOR3*)car_data[car].st.n.linearVelocity);
					SoundLogCarSample("siren", car_data[car].ap.model, sample & 0xff);
				}
				else
				{
					// play music
					siren_noise[j].chan = Start3DTrackingSound(-1, SOUND_BANK_ENVIRONMENT, 5,
						(VECTOR*)car_data[car].hd.where.t,
						(LONGVECTOR3*)car_data[car].st.n.linearVelocity);
				}

				LockChannel(siren_noise[j].chan);
				break;
			}
		}
	}

	// update sirens
	for (j = 0; j < MAX_SIREN_NOISES; j++)
	{
		if (siren_noise[j].in_use == 0)
			continue;

		car = siren_noise[j].car;

		if (car_data[car].controlType == CONTROL_TYPE_PURSUER_AI)
			pitch = car_data[car].ai.p.dying;
		else
			pitch = 0;

		SetChannelPosition3(siren_noise[j].chan,
			(VECTOR*)car_data[car].hd.where.t,
			(LONGVECTOR3*)car_data[car].st.n.linearVelocity,
			pitch * -30 - 3000, j * 4 - (pitch * 48 - 4096), 0);
	}

	// siren noises occupy car noise channels
	num_noisy_cars = MIN(num_noisy_cars, MAX_CAR_NOISES - sirens);

	car_flags = 0;

	for (j = 0; j < num_noisy_cars; j++)
	{
		car = indexlist[j];

		if (car_data[car].controlType != CONTROL_TYPE_PURSUER_AI || car_data[car].ai.p.dying == 0)
			car_flags |= 1 << car;
	}

	for (j = 0; j < MAX_CAR_NOISES; j++)
	{
		int noise;

		noise = (car_flags & (1 << car_noise[j].car)) != 0;
		car_noise[j].in_use = noise;

		car_flags &= ~(noise << car_noise[j].car);
	}

	for (j = 0; j < MAX_CAR_NOISES; j++)
	{
		if (car_noise[j].in_use == 0 && car_noise[j].stopped == 0)
		{
			StopChannel(car_noise[j].chan);
			UnlockChannel(car_noise[j].chan);

			car_noise[j].chan = -1;
			car_noise[j].car = MAX_CARS;
			car_noise[j].stopped = 1;
		}
	}

	// start new sounds
	// pick free car noise slot and play
	for (i = 0; i < num_noisy_cars; i++)
	{
		if (car_flags & 1 << indexlist[i])
		{
			car = indexlist[i];
			for (j = 0; j < MAX_CAR_NOISES; j++)
			{
				int bankStart, model;

				if (car_noise[j].in_use)
					continue;

				car_noise[j].in_use = 1;
				car_noise[j].stopped = 0;
				car_noise[j].car = car;

				// determine which sound type it has to play
				if (gInGameCutsceneActive != 0 && force_idle[car] > -1)
					car_noise[j].idle = force_idle[car];
				else
					car_noise[j].idle = (car_data[car].hd.speed < 17);

				model = car_data[car].ap.model;

				if (model == 3)
					model = cop_model;

				// get bank id
				bankStart = GetCarBankSample(model);

				if (car_noise[j].idle)
					sample = bankStart * 3 + 1;
				else
					sample = bankStart * 3;

				car_noise[j].chan = Start3DTrackingSound(-1, SOUND_BANK_CARS, sample, 
					(VECTOR*)car_data[car].hd.where.t, 
					(LONGVECTOR3*)car_data[car].st.n.linearVelocity);

				LockChannel(car_noise[j].chan);
				break;
			}
		}
	}


	// update sounds of cars (swap between idle and rev)
	for (j = 0; j < MAX_CAR_NOISES; j++)
	{
		CAR_DATA* cp;
		char old_idle;

		if (car_noise[j].in_use == 0)
			continue;

		car = car_noise[j].car;
		old_idle = car_noise[j].idle;
		cp = &car_data[car];

		// determine which sound type it has to play
		if (gInGameCutsceneActive != 0 && force_idle[car] > -1)
			car_noise[j].idle = force_idle[car];
		else
			car_noise[j].idle = (cp->hd.speed < 17);

		// restart sound if it's changed
		if (old_idle != car_noise[j].idle)
		{
			int bankStart, model;

			StopChannel(car_noise[j].chan);
			UnlockChannel(car_noise[j].chan);

			model = cp->ap.model;

			if (model == 3)
				model = cop_model;

			// get bank id
			bankStart = GetCarBankSample(model);

			if (car_noise[j].idle)
				sample = bankStart * 3 + 1;
			else
				sample = bankStart * 3;

			car_noise[j].chan = Start3DTrackingSound(-1, SOUND_BANK_CARS, sample, (VECTOR*)cp->hd.where.t, (LONGVECTOR3*)cp->st.n.linearVelocity);
			
			LockChannel(car_noise[j].chan);
		}

		if (car_data[car].controlType == CONTROL_TYPE_CIV_AI)
			volume = -7000;
		else
			volume = -6250;

		pitch = (car_data[car].hd.revs << 0x10) >> 0x12;

		if (car_noise[j].idle != 0)
			pitch += 4096;
		else
			pitch += 1500;

		car_noise[j].in_use = 1;

		SetChannelPosition3(car_noise[j].chan, (VECTOR*)cp->hd.where.t, (LONGVECTOR3*)cp->st.n.linearVelocity, volume, pitch, 0);
	}

	// bark on player
	if (CopsCanSeePlayer)
	{
		playerFelony = GetPlayerFelony(&MainPlayer);

		if (*playerFelony > FELONY_PURSUIT_MIN_VALUE)
			DoPoliceLoudhailer(num_noisy_cars, indexlist, car_dist);
	}

	// update each sound channel with new info
	CHANNEL_DATA* c = &channels[0];
	for (j = 0; j < MAX_SFX_CHANNELS; j++, c++)
	{
		if ((c->flags & CHAN_LOOP) == 0 && c->time != 0 && c->srcposition != NULL)
		{
			SetChannelPosition3(j, c->srcposition, c->srcvelocity, c->srcvolume, c->srcpitch, 0);
		}
	}
}

// [D] [T]
void DoPoliceLoudhailer(int cars, ushort* indexlist, u_int* dist)
{
	int rnd;
	int carId;
	int i;
	int time;

	if (!gDoCopSpeech)
		return;

	rnd = Random2(78);

	if (GameType == GAME_GETAWAY)
		time = 475;
	else
		time = 275;

	for (i = 0; i < cars; i++)
	{
		CAR_DATA* car_ptr;

		carId = indexlist[i];

		car_ptr = &car_data[carId];

		if (dist[carId] > 0x6000)
			dist[carId] -= 0x6000;

		if (car_ptr->controlType == CONTROL_TYPE_PURSUER_AI && car_ptr->ai.p.dying == 0 &&
			time < loudhail_time && rnd == (rnd / 31) * 31)
		{
			Start3DTrackingSound(-1, SOUND_BANK_VOICES, rnd % 2 + 13, 
				(VECTOR*)car_ptr->hd.where.t, 
				(LONGVECTOR3*)car_ptr->st.n.linearVelocity);
			
			loudhail_time = 0;
			break;
		}
	}

	if (loudhail_time <= time)
		loudhail_time++;
}


// [D] [T]
void CollisionSound(char player_id, CAR_DATA* cp, int impact, int car_car)
{
	int chan;
	int rnd;
	int playerid;
	int phrase;
	int sample;

	playerid = player_id < 0 ? 0 : player_id;

	if (impact < 25)
		return;

	// get player
	if (NumPlayers > 1 && NoPlayerControl == 0)
	{
		int dx, dz;
		u_int p0dst;
		u_int p1dst;

		dx = cp->hd.where.t[0] - MainPlayer.pos[0];
		dz = cp->hd.where.t[2] - MainPlayer.pos[2];

		p0dst = (dx * dx + dz * dz);

		dx = cp->hd.where.t[0] - SecondPlayer.pos[0];
		dz = cp->hd.where.t[2] - SecondPlayer.pos[2];

		p1dst = (dx * dx + dz * dz);

		playerid = p1dst < p0dst;
	}

	if (player[playerid].crash_timer)
		return;

	sample = 4;
	phrase = 0;

	if (car_car == 0)
	{
		if (impact > 780)
		{
			sample = 6;
			phrase = 1;
		}
		else if (impact > 350)
		{
			sample = 5;
			phrase = 2;
		}
	}
	else
	{
		if (impact > 900)
		{
			sample = 6;
			phrase = 1;
		}
		else if (impact > 380)
		{
			sample = 5;
			phrase = 2;
		}
	}

	chan = GetFreeChannel();

	if (NumPlayers > 1 && NoPlayerControl == 0)
		SetPlayerOwnsChannel(chan, playerid);
	else if(playerid > 0)
		return;
	
	Start3DSoundVolPitch(chan, SOUND_BANK_SFX, sample, cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], -2750, impact - (impact / 1024) * 1024 + 3584);

	player[playerid].crash_timer = 2;

	if ((impact & 5) && 
		GetPlayerId(cp) == 0 &&
		(gCurrentMissionNumber - 2 <= 2 || gCurrentMissionNumber == 9 || gCurrentMissionNumber == 10 || gCurrentMissionNumber == 27))
	{
		rnd = Random2(1);

		if (rnd == (rnd / 3) * 3)
		{
			phrase |= 4;
		}
		else
		{
			if (car_car != 2)
			{
				if (phrase != 0)
				{
					if ((Random2(1) & 1) == 0)
						sample = 3;
					else
						sample = 0;

					BodSay(sample);
				}

				return;
			}

			if (phrase == 0)
				return;

			if ((Random2(1) & 1) != 0)
				phrase = 1;
			else
				phrase = 2;
		}

		BodSay(phrase);
	}
}

// [D] [T]
void ExplosionSound(VECTOR* pos, int type)
{
	int rnd;
	int sc2;
	int sc1;
	int bang;
	VECTOR P;

	bang = 255;
	rnd = Random2(4);

	if (gCurrentMissionNumber == 13 || gCurrentMissionNumber == 23)
	{
		bang = GetMissionSound(12);
	}
	else if (gCurrentMissionNumber == 30 || gCurrentMissionNumber == 35)
	{
		bang = GetMissionSound(29);
	}

	if (bang == 255)
		bang = phrase_top;	// no mission-specific sound: use the generic explosion sample

	if (type == BIG_BANG)
	{
		sc1 = 2;
		sc2 = 2;
	}
	else if (type == LITTLE_BANG)
	{
		sc1 = 3;
		sc2 = 1;
	}
	else // HEY_MOMMA
	{
		sc1 = 1;
		sc2 = 3;
	}
	

	P.vx = pos->vx * sc1 + MainPlayer.cameraPos.vx * sc2;
	P.vy = pos->vy * sc1 + MainPlayer.cameraPos.vy * sc2;
	P.vz = pos->vz * sc1 + MainPlayer.cameraPos.vz * sc2;

	Start3DSoundVolPitch(-1, SOUND_BANK_MISSION,
		bang, P.vx / 4, P.vy / 4, P.vz / 4,
		0, ((FrameCnt * pos->vx ^ rnd * pos->vz) & 0x3ffU) + 3584);
}


// [D] [T]
void JerichoSpeak(void)
{
	static u_int j_said = 0;
	int rnd;
	short* playerFelony;

	rnd = Random2(3);

	if (CopsCanSeePlayer == 0)
		return;

	playerFelony = GetPlayerFelony(&MainPlayer);

	if (*playerFelony > FELONY_PURSUIT_MIN_VALUE && rnd == rnd / 5 * 5)
	{
		if (j_said > 60)
		{
			BodSay(rnd % 3);
			j_said = 0;
		}
		else
			j_said++;
	}
}

// [D] [T]
void FunkUpDaBGMTunez(int funk)
{
	if (funk == 0)
	{
		if (copmusic != 0)
		{
			copmusic = 0;
			Song_SetPos = 0;
		}
	}
	else
	{
		if (copmusic == 0)
		{
			copmusic = 1;
			Song_SetPos = xm_coptrackpos[current_music_id];
		}
	}
}

// [D] [T]
void SoundTasks(void)
{
	int chan;
	int vol;
	VECTOR* position;
	LONGVECTOR3* velocity;

	PLAYER* lcp;
	CAR_DATA* cp;
	int i;

	UpdateEnvSounds[EStags.func_cnt++](envsnd, ESdata, 0);
	EStags.func_cnt %= 4;
	
	lcp = player;
	i = 0;
	while (i < NumPlayers)
	{
		cp = lcp->playerCarId > -1 ? &car_data[lcp->playerCarId] : NULL;

		// doppler camera velocity
		if (cp && (lcp->cameraView == 2 || lcp->cameraView == 0))
		{
			lcp->camera_vel[0] = cp->st.n.linearVelocity[0];
			lcp->camera_vel[1] = cp->st.n.linearVelocity[1];
			lcp->camera_vel[2] = cp->st.n.linearVelocity[2];
		}
		else
		{
			lcp->camera_vel[0] = 0;
			lcp->camera_vel[1] = 0;
			lcp->camera_vel[2] = 0;
		}

		if (lcp->car_sound_timer > -1)
			lcp->car_sound_timer--;

		// play engine start sound
		if (cp && lcp->car_sound_timer == 4)
		{
			Start3DSoundVolPitch(-1, SOUND_BANK_TANNER, 4, cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], -2500, 3072);
		}

		if (lcp->car_sound_timer == 0)
			lcp->car_is_sounding = 0;

		if (lcp->crash_timer > 0)
			lcp->crash_timer--;

		// car engine sounds
		if (cp)
		{
			position = (VECTOR*)cp->hd.where.t;
			velocity = (LONGVECTOR3*)cp->st.n.linearVelocity;

			// JERICHO-HOOK: engine-audio tuners (cainescrossfire): scale/offset the
			// rev + idle channel pitch and volume before they are placed.
			{
				JER_ARGS_CAR_ENGINE_SOUND es;
				int rvol;
				int ivol;

				if (lcp->car_is_sounding < 2)
					rvol = lcp->revsvol;
				else
					rvol = -10000;

				if (lcp->car_is_sounding == 0)
					ivol = lcp->idlevol;
				else
					ivol = -10000;

				es.car = cp;
				es.playerId = i;
				es.revPitch = cp->hd.revs / 4 + lcp->revsvol / 64 + 1500;
				es.revVolume = rvol;
				es.idlePitch = cp->hd.revs / 4 + 4096;
				es.idleVolume = ivol;

				jer_fire(JER_EVENT_CAR_ENGINE_SOUND, &es);

				SetChannelPosition3(i * 3, position, velocity, es.revVolume, es.revPitch, 0);
				SetChannelPosition3(i * 3 + 1, position, velocity, es.idleVolume, es.idlePitch, 0);
			}

			// siren sound control
			if (CarHasSiren(cp->ap.model) != 0)
			{
				chan = i  * 3 + 2;

				if (lcp->horn.on == 0)
					SpuSetVoicePitch(chan, 0);		// don't stop it really
				else
					SetChannelPosition3(chan, position, velocity, 0, 4096, 0);
			}
		}
		else
		{
			chan = i * 3;
			SetChannelVolume(chan, -10000, 0);

			chan = i * 3 + 1;
			SetChannelVolume(chan, -10000, 0);

			chan = i * 3 + 2;
			SetChannelVolume(chan, -10000, 0);
		}

		lcp++;
		i++;
	}

	if (NumPlayers < 2 || NoPlayerControl != 0)
	{
		UpdateEnvSounds[EStags.func_cnt++](envsnd, ESdata, 0);
		EStags.func_cnt %= 4;

		DoDopplerSFX();
		Tunnels(&tunnels);
	}

	ControlSpeech(&gSpeechQueue);

	// various mission and cutscene stuff
	if (gInGameCutsceneActive != 0)
	{
		HandleRequestedXA();
		DoCutsceneSound();
	}
	else
	{
		DoMissionSound();
	}

	// do annoying lead car horn
	if (gInGameChaseActive != 0 && MainPlayer.targetCarId >= 0)
		LeadHorn(&car_data[MainPlayer.targetCarId]);		// use target id instead

	// FIXME: move it to MC_SND?
	if (jericho_in_back != 0 && (gCurrentMissionNumber == 20 || gCurrentMissionNumber == 25 || gCurrentMissionNumber == 39))
	{
		JerichoSpeak();
	}
}

// [D] [T]
void InitMusic(int musicnum)
{
	static char* music_pt; // offset 0xc
	static char* sample_pt; // offset 0x10
	static char xm_samples; // offset 0x4
	int sample_len;
	int music_len;
	char* name;
	char* addr;
	int musicpos[3];

	
	char* musicname = "SOUND\\MUSIC.BIN";

#ifndef PSX
	char* d1musicName = "SOUND\\D1MUSIC.BIN";
	// search for Driver 1 music file
	if (gDriver1Music && FileExists(d1musicName))
	{
		name = d1musicName;
		xm_coptrackpos = xm_coptrackpos_d1;
	}
	else
#endif
	{
		name = musicname;
		xm_coptrackpos = xm_coptrackpos_d2;
		gDriver1Music = 0;
	}

	copmusic = 0;

	AllocateReverb(3, 16384);

	current_music_id = musicnum;
	LoadfileSeg(name, (char*)musicpos, musicnum * 8, sizeof(musicpos));

	D_MALLOC_BEGIN()

	sample_len = musicpos[2] - musicpos[1];
	music_len = musicpos[1] - musicpos[0];

	if (NewLevel)
	{
		printInfo("NewLevel in InitMusic()\n");
		
		music_pt = D_MALLOC(music_len + 3U & ~3);
		sample_pt = D_TEMPALLOC(sample_len);

#if USE_CRT_MALLOC
		LoadfileSeg(name, music_pt, musicpos[0], music_len);
		LoadfileSeg(name, sample_pt, musicpos[0] + music_len, sample_len);
#else
		LoadfileSeg(name, music_pt, musicpos[0], music_len + sample_len);
#endif
	}

	if (Song_ID == -1)
	{
		VABID = XM_GetFreeVAB();

		if (NewLevel != 0)
		{
			xm_samples = LoadSoundBank(sample_pt, sample_len, 0);
			D_TEMPFREE();
		}

		UpdateXMSamples(xm_samples);

		XM_GetFileHeaderSize();
		XM_SetFileHeaderAddress((unsigned char*)header_pt);
		XM_GetSongSize();
		XM_SetSongAddress((unsigned char*)song_pt);
	}

	D_MALLOC_END();

	InitXMData((unsigned char*)music_pt, 0, 0);

	// 8 XM channels start after first sfx channels
	Song_ID = XM_Init(VABID, 0, 0, MAX_SFX_CHANNELS, 1, -1, 0x80, 0);

	if (music_paused != 0)
		XM_Pause(Song_ID);
}

// [D] [T]
void InitTunnels(char n)
{
	if (n > 29)
		n = 29;

	tunnels.num_tunnels = n;
	tunnels.tunnel_cnt = 0;
}


// [D] [T]
int AddTunnel(int x1, int y1, int z1, int x2, int y2, int z2)
{
	if (tunnels.tunnel_cnt >= tunnels.num_tunnels)
		return -1;

	tunnels.coords[tunnels.tunnel_cnt].p1.vx = x1;
	tunnels.coords[tunnels.tunnel_cnt].p1.vy = y1;
	tunnels.coords[tunnels.tunnel_cnt].p1.vz = z1;

	tunnels.coords[tunnels.tunnel_cnt].p2.vx = x2;
	tunnels.coords[tunnels.tunnel_cnt].p2.vy = y2;
	tunnels.coords[tunnels.tunnel_cnt].p2.vz = z2;

	return tunnels.tunnel_cnt++;
}

// [D] [T]
void Tunnels(tunnelinfo* T)
{
	int i;
	int verb;

	gTunnelNum = -1;
	NoRainIndoors = 0;
	verb = 0;

	i = 0;
	while (i < T->tunnel_cnt)
	{
		if (MIN(T->coords[i].p1.vx, T->coords[i].p2.vx) < camera_position.vx &&
			MAX(T->coords[i].p1.vx, T->coords[i].p2.vx) > camera_position.vx &&

			MIN(T->coords[i].p1.vy, T->coords[i].p2.vy) < camera_position.vy &&
			MAX(T->coords[i].p1.vy, T->coords[i].p2.vy) > camera_position.vy &&

			MIN(T->coords[i].p1.vz, T->coords[i].p2.vz) < camera_position.vz &&
			MAX(T->coords[i].p1.vz, T->coords[i].p2.vz) > camera_position.vz)
		{
			verb = 1;
			gTunnelNum = i;
			NoRainIndoors = 1;
			break;
		}

		i++;
	}

	SetReverbInGameState(verb);
}

// [D] [T]
void AddTunnels(int level)
{
	if (level == 0)
	{
		InitTunnels(29);

		AddTunnel(-78200, 0, -125000, -43800, -800, -159500);
		AddTunnel(-65000, 0, -122700, -68000, -800, -125000);
		AddTunnel(-48600, 0, -237000, -50350, -800, -235900);

		AddTunnel(-219895, 0, 675067, -208647, -2500, 668935);

		AddTunnel(-294644, 0, 319973, -288508, -800, 316917);
		AddTunnel(-282105, 0, 321774, -275985, -800, 315638);
		AddTunnel(-268534, 0, 320244, -262410, -800, 314176);
		AddTunnel(-293886, 0, 308728, -289806, -800, 305656);
		AddTunnel(-278274, 0, 308728, -277246, -800, 305664);
		AddTunnel(-298238, 0, 301568, -289018, -800, 298496);
		AddTunnel(-292102, 0, 298496, -289018, -800, 295416);
		AddTunnel(-283386, 0, 297460, -280314, -800, 294396);
		AddTunnel(-283390, 0, 294396, -274182, -800, 291328);
		AddTunnel(-290443, 0, 278252, -282239, -800, 275192);
		AddTunnel(-281338, 0, 276976, -278238, -800, 273920);
		AddTunnel(-288502, 0, 269048, -285434, -800, 265984);
		AddTunnel(-289411, 0, 259814, -288395, -800, 256762);
		AddTunnel(-297210, 0, 252666, -291090, -800, 249590);
		AddTunnel(-284414, 0, 252666, -278286, -800, 249590);
		AddTunnel(-268538, 0, 252666, -262398, -800, 249590);
		AddTunnel(-298498, 0, 240889, -290310, -800, 237825);
		AddTunnel(-266490, 0, 243189, -260358, -800, 240133);
		AddTunnel(-281846, 0, 233719, -280830, -800, 230655);
		AddTunnel(-280830, 0, 230655, -274690, -800, 227587);
		AddTunnel(-285438, 0, 207353, -284422, -800, 204285);
		AddTunnel(-292609, 0, 200184, -274181, -800, 194052);
		AddTunnel(-267521, 0, 198136, -264453, -800, 192000);
		AddTunnel(-268542, 0, 192000, -265477, -800, 185864);

		AddTunnel(-217585, 0, -352018, -199197, -800, -358146);
	}
	else if (level == 1)
	{
		InitTunnels(4);

		AddTunnel(-398650, 0, -161000, -340300, 2000, -103000);
		AddTunnel(272800, 0, 41200, 255000, 2000, 168600);
		AddTunnel(-477600, 0, -112700, -397000, 8000, -132800);
		AddTunnel(-185875, 0, -41450, -179650, -1000, -44600);
	}
	else if (level == 2)
	{
		InitTunnels(2);

		AddTunnel(157578, 0, 740250, 168330, -2000, 725915);
		AddTunnel(165200, 0, 725915, 168330, -2000, 722650);
	}
	else if (level == 3)
	{
		InitTunnels(5);

		AddTunnel(151400, 0, -250740, 154800, -400, -235750);
		AddTunnel(-115100, 0, -193200, -113300, -400, -132500);
		AddTunnel(-71900, 0, -16250, -73650, -400, -28500);
		AddTunnel(78300, 0, -167900, 82450, -500, -169950);
		AddTunnel(173550, 0, 247400, 177700, -500, 245300);
	}
}


// [D] [T]
void InitEnvSnd(int num_envsnds)
{
	int i, p;

	if (num_envsnds > MAX_LEVEL_ENVSOUNDS)
		num_envsnds = MAX_LEVEL_ENVSOUNDS;

	for (i = 0; i < num_envsnds; i++)
		envsnd[i].type = 0;

	for (i = 0; i < NumPlayers; i++)
	{
		for (p = 0; p < 4; p++)
		{
			ESdata[i].playing_sound[p] = -1;
			ESdata[i].thisS[p] = -1;
		}
	}

	EStags.frame_cnt = 0;
	EStags.func_cnt = 0;
	EStags.num_envsnds = num_envsnds;
	EStags.envsnd_cnt = 0;
}

// [D] [T]
int SetEnvSndVol(int snd, int vol)
{
	int was;

	was = envsnd[snd].vol;
	envsnd[snd].vol = vol;

	return was;
}

// [D] [T]
void SetEnvSndPos(int snd, int px, int pz)
{
	if (envsnd[snd].type != 3)
		return;

	envsnd[snd].pos2.vx = px;
	envsnd[snd].pos.vx = px;
	envsnd[snd].pos2.vz = pz;
	envsnd[snd].pos.vz = pz;
}

// [D] [T]
int AddEnvSnd(int type, char flags, int bank, int sample, int vol, int px, int pz, int px2, int pz2)
{
	envsound* ep;

	if (EStags.envsnd_cnt >= EStags.num_envsnds)
		return -1;

	ep = &envsnd[EStags.envsnd_cnt];

	ep->type = type;
	ep->bank = bank;
	ep->sample = sample;
	ep->flags = flags;
	ep->vol = vol;

	switch (type)
	{
		case 1:
		case 2:
		case 4:
			ep->pos.vx = px;
			ep->pos.vz = pz;

			ep->pos2.vx = px2;
			ep->pos2.vz = pz2;
			break;
		case 3:
			ep->pos.vx = px;
			ep->pos.vz = pz;

			ep->pos2.vx = px;
			ep->pos2.vz = pz;
			break;
		case 5:
			ep->type = 1;

			ep->pos.vx = px - px2;
			ep->pos.vz = pz - px2;

			ep->pos2.vx = px + px2;
			ep->pos2.vz = pz + px2;
	}

	if (ep->pos.vx == ep->pos2.vx)
		ep->flags |= 1;

	if (ep->pos.vz == ep->pos2.vz)
		ep->flags |= 2;

	if (type != 3)
		ep->flags |= 0x20;

	return EStags.envsnd_cnt++;
}

// It computes a fast 1 / sqrtf(v) approximation
inline float rsqrtf(float v)
{
	float v_half = v * 0.5f;
	int i = *(int*)&v;
	i = 0x5f3759df - (i >> 1);
	v = *(float*)&i;
	return v * (1.5f - v_half * v * v);
}

// [D] [T]
void IdentifyZone(envsound* ep, envsoundinfo* E, int pl)
{
	int i, j;
	int vol;

	int tmp[4];
	float _g[4];
	bitfield64 zones;
	int snd;

	zones.l = 0;
	zones.h = 0;

	for (i = 0; i < 4; i++)
		tmp[i] = -1;

	E->cam_pos.vx = player[pl].cameraPos.vx;
	E->cam_pos.vy = player[pl].cameraPos.vy;
	E->cam_pos.vz = player[pl].cameraPos.vz;

	j = 0;
	for (i = 0; i < EStags.envsnd_cnt && tmp[3] < 0; i++)
	{
		if (ep[i].type == 0 || ep[i].vol <= -7500)
			continue;

		vol = (ep[i].vol + 7500) * 4;
		vol = MAX(MIN(vol, 22000), 0);

		if (MIN(ep[i].pos.vx, ep[i].pos2.vx) - vol < E->cam_pos.vx &&
			MAX(ep[i].pos.vx, ep[i].pos2.vx) + vol > E->cam_pos.vx &&

			MIN(ep[i].pos.vz, ep[i].pos2.vz) - vol < E->cam_pos.vz &&
			MAX(ep[i].pos.vz, ep[i].pos2.vz) + vol > E->cam_pos.vz)
		{
			if ((ep[i].type == 2 || ep[i].type == 4) && (ep[i].flags & 3) == 0)
			{
				// [A] new distance estimation code, should be efficient enough
				// Plane method

				// build a plane from line segment
				float ldx, ldz, ndx, ndz, dist, l_inv_len, offset;
				ldx = ep[i].pos2.vx - ep[i].pos.vx;
				ldz = ep[i].pos2.vz - ep[i].pos.vz;
				
				// find inverse length of line
				l_inv_len = rsqrtf(ldx * ldx + ldz * ldz);
				
				// find normal (perpendicular) by using cross product and normalize
				ndx = ldz * l_inv_len;
				ndz = -ldx * l_inv_len;

				// line pos1 offset along normal
				offset = ndx * ep[i].pos.vx + ndz * ep[i].pos.vz;

				dist = ndx * E->cam_pos.vx + ndz * E->cam_pos.vz - offset;

				if (ep->type == 2)
				{
					if (dist < 0)
						dist = -dist;
				}
				else if ((ep->flags & 8) != 0) // flipped flag
				{
					dist = -dist;
				}

				if (dist < vol)
				{
					if (i < 32)
						zones.l |= 1 << (i & 31);
					else
						zones.h |= 1 << (i & 31);

					tmp[j] = i;
					
					// also store length
					_g[j] = l_inv_len;
					j++;
				}
			}
			else
			{
				if (i < 32)
					zones.l |= 1 << (i & 31);
				else
					zones.h |= 1 << (i & 31);

				tmp[j] = i;
				j++;
			}
		}
	}

	// off the sound
	for (i = 0; i < 4; i++)
	{
		snd = E->thisS[i];

		if (snd == -1)
			continue;

		if (snd < 32)
		{
			if ((zones.l & 1 << (snd & 0x1f)) == 0)
			{
				E->thisS[i] = -1;
				continue;
			}
		}
		else
		{
			if ((zones.h & 1 << (snd & 0x1f)) == 0)
			{
				E->thisS[i] = -1;
				continue;
			}
		}

		if (snd < 32)
			zones.l &= ~(1 << (snd & 0x1f));
		else
			zones.h &= ~(1 << (snd & 0x1f));
	}

	// assign to the envsound
	for (i = 0; i < 4; i++)
	{
		snd = tmp[i];

		if (snd == -1)
			continue;

		if (snd < 32)
			snd = zones.l & 1 << (snd & 0x1f);
		else
			snd = zones.h & 1 << (snd & 0x1f);

		if (snd == 0)
			continue;

		// assign free slot
		for (j = 0; j < 4; j++)
		{
			if (E->thisS[j] < 0)
			{
				E->thisS[j] = tmp[i];
				E->g[j] = _g[i];
				break;
			}
		}
	}
}


// [D] [T]
void CalcEffPos(envsound* ep, envsoundinfo* E, int pl)
{
	int minX, maxX;
	int minZ, maxZ;
	int i;

	for (i = 0; i < 4; i++)
	{
		int snd;
		
		E->flags &= ~(1 << (i & 0x1f));
	
		snd = E->thisS[i];
		
		if (snd == -1)
			continue;

		// BOX type
		if (ep[snd].type == 1)
		{
			// clamp to bounds when outside
			minX = MIN(ep[snd].pos.vx, ep[snd].pos2.vx);
			maxX = MAX(ep[snd].pos.vx, ep[snd].pos2.vx);
			
			if (minX > E->cam_pos.vx)
				E->eff_pos[i].vx = minX;
			else if (maxX < E->cam_pos.vx)
				E->eff_pos[i].vx = maxX;
			else
				E->eff_pos[i].vx = E->cam_pos.vx;

			minZ = MIN(ep[snd].pos.vz, ep[snd].pos2.vz);
			maxZ = MAX(ep[snd].pos.vz, ep[snd].pos2.vz);

			if (minZ > E->cam_pos.vz)
				E->eff_pos[i].vz = minZ;
			else if (maxZ < E->cam_pos.vz)
				E->eff_pos[i].vz = maxZ;
			else
				E->eff_pos[i].vz = E->cam_pos.vz;
		}
		else if (ep[snd].type == 2)
		{
			// point type
			// really can be a line in one axis (which is not used)
			if ((ep[snd].flags & 1) != 0)
			{
				E->eff_pos[i].vx = ep[snd].pos.vx;
				
				minZ = MIN(ep[snd].pos.vz, ep[snd].pos2.vz);
				maxZ = MAX(ep[snd].pos.vz, ep[snd].pos2.vz);

				if (minZ > E->cam_pos.vz)
					E->eff_pos[i].vz = minZ;
				else if (maxZ < E->cam_pos.vz)
					E->eff_pos[i].vz = maxZ;
				else
					E->eff_pos[i].vz = E->cam_pos.vz;
			}
			else if ((ep[snd].flags & 2) != 0)
			{
				E->eff_pos[i].vz = ep[snd].pos.vz;

				minX = MIN(ep[snd].pos.vx, ep[snd].pos2.vx);
				maxX = MAX(ep[snd].pos.vx, ep[snd].pos2.vx);

				if (minX > E->cam_pos.vx)
					E->eff_pos[i].vx = minX;
				else if (maxX < E->cam_pos.vx)
					E->eff_pos[i].vx = maxX;
				else
					E->eff_pos[i].vx = E->cam_pos.vx;
			}
			else
			{
				// type 2 and type 4 (floating point ones) are processed in CalcEffPos2
				// which are unused, too
				continue;
			}
		}
		else
			continue;

		E->flags |= 1 << (i & 0x1f);
	}
}

// [D] [T]
void CalcEffPos2(envsound* ep, envsoundinfo* E, int pl)
{
	int snd;
	int i;

	for (i = 0; i < 4; i++)
	{
		snd = E->thisS[i];

		if (snd > -1 && (E->flags & 1 << i) == 0)
		{
			if (ep[snd].type == 2 || ep[snd].type == 4)
			{
				// [A] determine sound position on line
				// Plane method

				// build a plane from line segment
				float ldx, ldz, ndx, ndz, dist, l_inv_len, offset;
				ldx = ep[snd].pos2.vx - ep[snd].pos.vx;
				ldz = ep[snd].pos2.vz - ep[snd].pos.vz;

				// find inverse length of line
				l_inv_len = E->g[i];

				// find normal (perpendicular) by using cross product and normalize
				ndx = ldz * l_inv_len;
				ndz = -ldx * l_inv_len;

				// line pos1 offset along normal
				offset = ndx * ep[snd].pos.vx + ndz * ep[snd].pos.vz;

				// now need distance
				dist = ndx * E->cam_pos.vx + ndz * E->cam_pos.vz - offset;

				// clamp to side
				E->eff_pos[i].vx = E->cam_pos.vx - ndx * dist;
				E->eff_pos[i].vz = E->cam_pos.vz - ndz * dist;

				if (ep[snd].type == 2)
				{
					int minX, maxX, minZ, maxZ;
					
					maxX = MAX(ep[snd].pos.vx, ep[snd].pos2.vx);
					minX = MIN(ep[snd].pos.vx, ep[snd].pos2.vx);

					if (E->eff_pos[i].vx > maxX)
					{
						E->eff_pos[i].vx = maxX;
					}
					else if (E->eff_pos[i].vx < minX)
					{
						E->eff_pos[i].vx = minX;
					}

					maxZ = MAX(ep[snd].pos.vz, ep[snd].pos2.vz);
					minZ = MIN(ep[snd].pos.vz, ep[snd].pos2.vz);

					if (E->eff_pos[i].vz > maxZ)
					{
						E->eff_pos[i].vz = maxZ;
					}
					else if (E->eff_pos[i].vz < minZ)
					{
						E->eff_pos[i].vz = minZ;
					}
					
					E->flags |= 1 << (i & 0x1f);
				}
				else if (ep[snd].type == 4)
				{
					if ((ep[snd].flags & 8) == 0) 
					{
						int minX, maxX, maxZ;

						maxZ = MAX(ep[snd].pos.vz, ep[snd].pos2.vz);
						
						if (E->cam_pos.vz > maxZ)
						{
							E->eff_pos[i].vz = maxZ;
						}
						else if (E->cam_pos.vz > E->eff_pos[i].vz)
						{
							E->eff_pos[i].vz = E->cam_pos.vz;
						}

						// clamp axis select based on distance sign
						if (E->g[i] < 0)
						{
							maxX = MAX(ep[snd].pos.vx, ep[snd].pos2.vx);

							if (E->cam_pos.vx > maxX)
							{
								E->eff_pos[i].vx = maxX;
							}
							else if (E->cam_pos.vx > E->eff_pos[i].vx) 
							{
								E->eff_pos[i].vx = (E->cam_pos).vx;
							}
						}
						else
						{
							minX = MIN(ep[snd].pos.vx, ep[snd].pos2.vx);
							
							if (minX > E->cam_pos.vx)
							{
								E->eff_pos[i].vx = minX;
							}
							else if (E->cam_pos.vx < E->eff_pos[i].vx)
							{
								E->eff_pos[i].vx = E->cam_pos.vx;
							}
						}
					}
					else
					{
						int minX, maxX, minZ;

						minZ = MIN(ep[snd].pos.vz, ep[snd].pos2.vz);
						
						if (E->cam_pos.vz < minZ)
						{
							E->eff_pos[i].vz = minZ;
						}
						else if (E->cam_pos.vz < E->eff_pos[i].vz)
						{
							E->eff_pos[i].vz = E->cam_pos.vz;
						}

						// clamp axis select based on distance sign
						if (E->g[i] > 0)
						{
							maxX = MAX(ep[snd].pos.vx, ep[snd].pos2.vx);

							if (E->cam_pos.vx > maxX)
							{
								E->eff_pos[i].vx = maxX;
							}
							else if (E->cam_pos.vx > E->eff_pos[i].vx) 
							{
								E->eff_pos[i].vx = E->cam_pos.vx;
							}
						}
						else
						{
							minX = MIN(ep[snd].pos.vx, ep[snd].pos2.vx);
							
							if (E->cam_pos.vx < minX)
							{
								E->eff_pos[i].vx = minX;
							}
							else if (E->cam_pos.vx < E->eff_pos[i].vx)
							{
								E->eff_pos[i].vx = E->cam_pos.vx;
							}
						}
					}

					
				}
			}
			else
			{
				E->eff_pos[i].vx = ep[snd].pos.vx;
				E->eff_pos[i].vz = ep[snd].pos.vz;
				
				E->flags |= (1 << i);
			}
		}

		E->eff_pos[i].vy = -E->cam_pos.vy;
	}

}

// [D] [T]
void UpdateEnvSnd(envsound* ep, envsoundinfo* E, int pl)
{
	int channel;
	LONGVECTOR3* velocity;
	int i;
	int snd;

	for (i = 0; i < 4; i++)
	{
		snd = E->thisS[i];

		if (snd >= 0)
		{
			// start the new sound if it's changed
			if (E->playing_sound[i] != snd)
			{
				channel = Start3DSoundVolPitch(-1, ep[snd].bank, ep[snd].sample, E->eff_pos[i].vx, E->eff_pos[i].vy, E->eff_pos[i].vz, 0, 4096);
				E->chan[i] = channel;

				LockChannel(channel);

				if (NumPlayers > 1 && NoPlayerControl == 0)
					SetPlayerOwnsChannel(channel, pl);

				E->playing_sound[i] = snd;
			}

			if (ep[snd].flags & 0x10)
				velocity = (LONGVECTOR3*)player[pl].camera_vel;
			else
				velocity = NULL;

			SetChannelPosition3(E->chan[i], &E->eff_pos[i], velocity, ep[snd].vol - 2500, 4096, ep[snd].flags & 0x20);
		}
		else
		{
			if (E->playing_sound[i] > -1)
			{
				StopChannel(E->chan[i]);
				UnlockChannel(E->chan[i]);
				
				E->playing_sound[i] = -1;
			}
		}
	}
}

unsigned int horn_time;

// [D] [T]
void InitLeadHorn(void)
{
	// [A] disable horns in some missions
	switch (gCurrentMissionNumber)
	{
		case 4:		// Tailing the drop
		case 10:	// Follow up the lead
		case 18:	// Tail Jericho
		case 26:	// Steal the ambulance
			horn_time = UINT_MAX;
			break;
		default:
			horn_time = 0;
	}
}


// [D] [T]
void LeadHorn(CAR_DATA* cp)
{
	static unsigned int rnd = 0;
	int carBank;
	int dx,dz;

	// [A] disabled horn in those missions
	if (horn_time == UINT_MAX)
		return;

	/* A siren car's third sample is its siren, not a horn. Cops are heard
	 * through DoDopplerSFX's siren loop, so never play it as a lead horn. */
	if (CarHasSiren(cp->ap.model) != 0)
		return;

	// [A] do not horn if too far from camera
	dx = cp->hd.where.t[0] - camera_position.vx >> 8;
	dz = cp->hd.where.t[2] - camera_position.vz >> 8;

	if (ABS(dx) > 64 || ABS(dz) > 64)
		return;

	if (horn_time == 0)
		rnd = (cp->hd.where.t[0] ^ cp->hd.where.t[2]) * (FrameCnt ^ cp->hd.where.t[1]) & 0x7f;

	horn_time++;

	if (horn_time == rnd)
	{
		carBank = GetCarBankSample(cp->ap.model);

		Start3DTrackingSound(-1, SOUND_BANK_CARS, carBank * 3 + 2, 
			(VECTOR*)cp->hd.where.t, 
			(LONGVECTOR3*)cp->st.n.linearVelocity);
		SoundLogCarSample("horn", cp->ap.model, carBank * 3 + 2);

		horn_time = 0;
	}
}





