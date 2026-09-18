// cainescrossfireenginesnd.c — Combat D2: the engine rev + idle channel tuner.
//
// (Split out of the old cainescrossfiremedia.c; see cainescrossfire_internal.h for the file
// map.)
//
// JER_EVENT_CAR_ENGINE_SOUND scales/offsets the player car's rev + idle channel
// pitch and volume (CD2_SND_* in cainescrossfire.h), and JER_EVENT_CAR_REVS scales how
// fast the pitch climbs toward its target and how hard it falls on shifts
// (CD2_REV_RISE_SCALE / CD2_REV_DROP_SCALE). Player cars only.

#include "driver2.h"
#include "cainescrossfire.h"
#include "cars.h"
#include "jericho.h"
#include "jer_events.h"
#include "turbo/turbo.h"		/* cd2TurboActive - the boost is audible */

// ENGINE SOUND (JER_EVENT_CAR_ENGINE_SOUND, gamesnd.c SoundTasks): scale and
// offset the player car's rev + idle channel pitch and volume. Tuners live in
// cainescrossfire.h as CD2_SND_*; see the header guide for units and directions.
static int cd2mOnCarEngineSound(void* ud, void* args)
{
	JER_ARGS_CAR_ENGINE_SOUND* e = (JER_ARGS_CAR_ENGINE_SOUND*)args;
	long long p;
	long long v;
	(void)ud;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	p = ((long long)e->revPitch * CD2_SND_PITCH_SCALE) >> 12;
	e->revPitch = (int)p + CD2_SND_PITCH_BIAS;

	/* TURBO: the boost is audible - the engine note rides higher while it lasts,
	 * on top of the over-rev the gearbox allows. */
	{
		CAR_DATA* cp = (CAR_DATA*)e->car;

		if (cp != NULL && cd2TurboActive(cp->id))
			e->revPitch += CD2_TURBO_PITCH_BOOST;
	}

	p = ((long long)e->idlePitch * CD2_SND_PITCH_SCALE) >> 12;
	e->idlePitch = (int)p + CD2_SND_IDLE_PITCH_BIAS;

	// Volume is PSX attenuation (0 loudest, -10000 silent): gain divides the
	// attenuation (8192 ≈ twice as loud), bias moves it toward 0 (louder).
	v = -e->revVolume;
	if (CD2_SND_REV_GAIN > 0 && v > 0)
		v = (v * 4096) / CD2_SND_REV_GAIN;
	v = -v + CD2_SND_REV_BIAS;
	if (v < CD2_SND_MIN_VOL) v = CD2_SND_MIN_VOL;
	if (v > CD2_SND_MAX_VOL) v = CD2_SND_MAX_VOL;
	e->revVolume = (int)v;

	v = -e->idleVolume;
	if (CD2_SND_IDLE_GAIN > 0 && v > 0)
		v = (v * 4096) / CD2_SND_IDLE_GAIN;
	v = -v + CD2_SND_IDLE_BIAS;
	if (v < CD2_SND_MIN_VOL) v = CD2_SND_MIN_VOL;
	if (v > CD2_SND_MAX_VOL) v = CD2_SND_MAX_VOL;
	e->idleVolume = (int)v;

	return JER_RESULT_CONTINUE;
}

// REV SLEW (JER_EVENT_CAR_REVS, gamesnd.c ControlCarRevs): scale how fast the
// engine pitch climbs toward its target revs (CD2_REV_RISE_SCALE) and how
// hard it falls on shifts / let-off (CD2_REV_DROP_SCALE). Player cars only.
static int cd2mOnCarRevs(void* ud, void* args)
{
	JER_ARGS_CAR_REVS* r = (JER_ARGS_CAR_REVS*)args;
	CAR_DATA* cp = (CAR_DATA*)r->car;
	(void)ud;

	if (!gCd2Cfg.enabled || cp->controlType != CONTROL_TYPE_PLAYER)
		return JER_RESULT_CONTINUE;

	r->revRise = (int)(((long long)r->revRise * CD2_REV_RISE_SCALE) >> 12);
	r->revDrop = (int)(((long long)r->revDrop * CD2_REV_DROP_SCALE) >> 12);

	if (r->revRise < 0) r->revRise = 0;
	if (r->revDrop < 0) r->revDrop = 0;

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Registration (called once by jer_module_cainescrossfire_entry in cainescrossfire.c)
// ---------------------------------------------------------------------------
void cd2EngineSndRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_ENGINE_SOUND, cd2mOnCarEngineSound, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_REVS, cd2mOnCarRevs, NULL, 0);

	ctx->jer_log(ctx, "[cainescrossfire] engine sound registered (SDK v%d)\n", ctx->sdkVersion);
}
