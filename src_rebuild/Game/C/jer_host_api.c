/*
 * jer_host_api.c -- the JERICHO host side of the stable addon API.
 *
 * The table is data: addons resolve `jer_host_api` at runtime and call the
 * function pointers. Nothing here is exported except that resolver, so an
 * addon has no import-library dependency on the game exe.
 */

#define JERICHO_HOST_BUILD 1

#include "driver2.h"
#include "main.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"
#include "jer_console.h"
#include "jer_hud.h"
#include "jer_text.h"
#include "jer_sound.h"
#include "jer_pause_menu.h"
#include "jer_frontend.h"
#include "jer_map.h"
#include "jer_notify.h"
#include "jer_host_api.h"

#include "cars.h"
#include "players.h"
#include "camera.h"
#include "civ_ai.h"
#include "pad.h"
#include "dr2roads.h"
#include "handling.h"
#include "map.h"
#include "models.h"
#include "objcoll.h"
#include "mission.h"
#include "felony.h"
#include "cosmetic.h"
#include "texture.h"
#include "system.h"

/* Frontend car-list tables: the store/availability view a module needs without
 * importing the engine data globals. */
extern int CarAvailability[CITY_COUNT][10];
extern char carNumLookup[CITY_COUNT][10];

#include <string.h>

/* The debug-overlay line helpers are C++ references in the engine. */
extern void Debug_AddLine(VECTOR& a, VECTOR& b, CVECTOR& colour);
extern void Debug_AddLineDepth(VECTOR& a, VECTOR& b, CVECTOR& colour);

/* ------------------------------------------------------------------ */
/* Small conversions                                                   */
/* ------------------------------------------------------------------ */

static void hostSetVec(JER_VEC* out, int x, int y, int z)
{
	out->x = x;
	out->y = y;
	out->z = z;
	out->pad = 0;
}

static void hostSetSvec(JER_SVEC* out, int x, int y, int z)
{
	out->x = (short)x;
	out->y = (short)y;
	out->z = (short)z;
	out->pad = 0;
}

static VECTOR hostVector(const JER_VEC* in)
{
	VECTOR v;

	v.vx = in->x;
	v.vy = in->y;
	v.vz = in->z;
	v.pad = 0;
	return v;
}

static CVECTOR hostCvector(const JER_CVEC* in)
{
	CVECTOR c;

	c.r = in->r;
	c.g = in->g;
	c.b = in->b;
	c.cd = in->code;
	return c;
}

static int hostCarSlot(const void* car)
{
	const CAR_DATA* cp = (const CAR_DATA*)car;

	if (cp < car_data || cp >= car_data + MAX_CARS)
		return -1;

	return (int)(cp - car_data);
}

/* ------------------------------------------------------------------ */
/* Core / config                                                       */
/* ------------------------------------------------------------------ */

/* jer_log, jer_fire and jer_override have the exact signatures the table
 * declares, so the table points at them directly. */

static const JER_HOST_CORE_API gCore =
{
	sizeof(JER_HOST_CORE_API), JER_HOST_API_VERSION,
	jer_log, jer_fire, jer_override
};

static const JER_HOST_CONFIG_API gConfig =
{
	sizeof(JER_HOST_CONFIG_API), JER_HOST_API_VERSION,
	jer_config_get_int, jer_config_set_int,
	jer_config_get_bool, jer_config_set_bool,
	jer_config_get_str, jer_config_set_str
};

/* ------------------------------------------------------------------ */
/* Car                                                                 */
/* ------------------------------------------------------------------ */

static int hostCarCount(void)
{
	return MAX_CARS;
}

static void* hostCarAt(int slot)
{
	if (slot < 0 || slot >= MAX_CARS)
		return NULL;

	return &car_data[slot];
}

static int hostCarSlotUsable(int slot)
{
	return JerCarSlotUsable(slot);
}

static int hostCarWheelMask(void* car)
{
	CAR_DATA* cp = (CAR_DATA*)car;
	int i;
	int mask = 0;

	if (cp == NULL)
		return 0;

	for (i = 0; i < 4; i++)
	{
		if (cp->hd.wheel[i].susCompression != 0)
			mask |= 1 << i;
	}

	return mask;
}

static int hostCarGetState(void* car, JER_CAR_STATE* out)
{
	CAR_DATA* cp = (CAR_DATA*)car;
	int i;

	if (cp == NULL || out == NULL)
		return 0;

	memset(out, 0, sizeof(*out));
	out->size = sizeof(*out);
	out->carId = hostCarSlot(car);

	hostSetVec(&out->position, cp->st.n.fposition[0], cp->st.n.fposition[1], cp->st.n.fposition[2]);
	out->orientation.x = cp->st.n.orientation[0];
	out->orientation.y = cp->st.n.orientation[1];
	out->orientation.z = cp->st.n.orientation[2];
	out->orientation.w = cp->st.n.orientation[3];
	hostSetVec(&out->velocity, cp->st.n.linearVelocity[0], cp->st.n.linearVelocity[1], cp->st.n.linearVelocity[2]);
	hostSetVec(&out->angularVelocity, cp->st.n.angularVelocity[0], cp->st.n.angularVelocity[1], cp->st.n.angularVelocity[2]);

	out->speed = cp->hd.speed;
	out->wheelSpeed = cp->hd.wheel_speed;
	out->wheelMask = hostCarWheelMask(car);
	out->controlType = cp->controlType;
	out->controlFlags = cp->controlFlags;
	out->model = cp->ap.model;
	out->palette = cp->ap.palette;
	out->damage = cp->totalDamage;
	hostSetSvec(&out->wheelAngle, cp->wheel_angle, 0, 0);
	out->thrust = cp->thrust;
	out->revs = cp->hd.revs;
	out->gear = cp->hd.gear;

	out->playerId = -1;

	for (i = 0; i < MAX_PLAYERS; i++)
	{
		if (player[i].playerCarId == (char)out->carId)
		{
			out->playerId = i;
			break;
		}
	}

	return 1;
}

static int hostCarSetState(void* car, const JER_CAR_STATE* in)
{
	CAR_DATA* cp = (CAR_DATA*)car;

	if (cp == NULL || in == NULL || in->size < sizeof(JER_CAR_STATE))
		return 0;

	cp->st.n.fposition[0] = in->position.x;
	cp->st.n.fposition[1] = in->position.y;
	cp->st.n.fposition[2] = in->position.z;
	cp->st.n.orientation[0] = in->orientation.x;
	cp->st.n.orientation[1] = in->orientation.y;
	cp->st.n.orientation[2] = in->orientation.z;
	cp->st.n.orientation[3] = in->orientation.w;
	cp->st.n.linearVelocity[0] = in->velocity.x;
	cp->st.n.linearVelocity[1] = in->velocity.y;
	cp->st.n.linearVelocity[2] = in->velocity.z;
	cp->st.n.angularVelocity[0] = in->angularVelocity.x;
	cp->st.n.angularVelocity[1] = in->angularVelocity.y;
	cp->st.n.angularVelocity[2] = in->angularVelocity.z;

	cp->hd.speed = in->speed;
	cp->hd.wheel_speed = in->wheelSpeed;
	cp->controlType = (u_char)in->controlType;
	cp->controlFlags = (u_char)in->controlFlags;
	cp->ap.model = (char)in->model;
	cp->ap.palette = (char)in->palette;
	cp->totalDamage = (u_short)in->damage;
	cp->wheel_angle = (short)in->wheelAngle.x;
	cp->thrust = (short)in->thrust;
	cp->hd.revs = (short)in->revs;
	cp->hd.gear = (char)in->gear;

	return 1;
}

static void* hostCarModel(void* car)
{
	CAR_DATA* cp = (CAR_DATA*)car;

	if (cp == NULL || cp->ap.model < 0 || cp->ap.model >= MAX_CAR_RESIDENT_MODELS)
		return NULL;

	return gCarCleanModelPtr[cp->ap.model];
}

static void* hostCarCosmetic(void* car)
{
	CAR_DATA* cp = (CAR_DATA*)car;
	return (cp != NULL) ? (void*)cp->ap.carCos : NULL;
}

static int hostCarModelIndex(void* car)
{
	CAR_DATA* cp = (CAR_DATA*)car;
	return (cp != NULL) ? (int)cp->ap.model : -1;
}

static int hostCarInit(int slot, int model, const JER_VEC* position, const JER_SVEC* orientation)
{
	CAR_DATA* cp;
	LONGVECTOR4 pos;

	if (slot < 0 || slot >= MAX_CARS || position == NULL)
		return 0;

	cp = &car_data[slot];
	pos[0] = position->x;
	pos[1] = position->y;
	pos[2] = position->z;
	pos[3] = 0;

	return InitCar(cp, (orientation != NULL) ? orientation->y : 0, &pos,
		CONTROL_TYPE_CIV_AI, model, 0, NULL);
}

static int hostCarMapHeight(const JER_VEC* position)
{
	VECTOR v;

	if (position == NULL)
		return 0;

	v = hostVector(position);
	return MapHeight(&v);
}

static int hostCarRoadInfo(int x, int y, int z, JER_HOST_ROAD_INFO* out)
{
	JER_ROAD_INFO ri;

	if (out == NULL)
		return 0;

	memset(out, 0, sizeof(*out));

	if (!JerRoadInfoAt(x, y, z, &ri))
		return 0;

	out->surfId = ri.surfId;
	out->kind = ri.kind;
	out->numLanes = ri.numLanes;
	out->speedLimit = ri.speedLimit;
	out->laneDirs = ri.laneDirs;
	out->aiLanes = ri.aiLanes;
	out->connect[0] = ri.connect[0];
	out->connect[1] = ri.connect[1];
	out->connect[2] = ri.connect[2];
	out->connect[3] = ri.connect[3];
	return 1;
}

static int hostCarCellEmpty(const JER_VEC* position, int flags)
{
	VECTOR v;

	if (position == NULL)
		return 0;

	v = hostVector(position);
	return CellEmpty(&v, flags);
}

static int hostCarActiveSlots(int* out, int max)
{
	int i;
	int n = 0;

	if (out == NULL || max <= 0)
		return 0;

	for (i = 0; i < MAX_CARS && n < max; i++)
	{
		int slot;

		if (active_car_list[i] == NULL)
			continue;

		slot = hostCarSlot(active_car_list[i]);
		if (slot >= 0)
			out[n++] = slot;
	}

	return n;
}

/* Camera-side and physics-side views a compiled-in module used to reach
 * through CAR_DATA. Kept behind opaque car tokens and in/out structs. */
static int hostCarGetMatrix(void* car, JER_MATRIX* out)
{
	CAR_DATA* cp = (CAR_DATA*)car;
	int i, j;

	if (cp == NULL || out == NULL)
		return 0;

	memset(out, 0, sizeof(*out));
	out->size = sizeof(*out);

	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			out->m[i][j] = cp->hd.where.m[i][j];

	hostSetVec(&out->t, cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2]);
	return 1;
}

static int hostCarRebuildMatrix(void* car)
{
	CAR_DATA* cp = (CAR_DATA*)car;

	if (cp == NULL)
		return 0;

	RebuildCarMatrix(&cp->st, cp);
	return 1;
}

static int hostCarAddAngularAccel(void* car, const JER_VEC* aacc)
{
	CAR_DATA* cp = (CAR_DATA*)car;

	if (cp == NULL || aacc == NULL)
		return 0;

	cp->hd.aacc[0] += aacc->x;
	cp->hd.aacc[1] += aacc->y;
	cp->hd.aacc[2] += aacc->z;
	return 1;
}

static int hostCarWheelState(void* car, int wheel, JER_WHEEL_STATE* out)
{
	CAR_DATA* cp = (CAR_DATA*)car;

	if (cp == NULL || out == NULL || wheel < 0 || wheel >= 4)
		return 0;

	memset(out, 0, sizeof(*out));
	out->size = sizeof(*out);
	out->compression = cp->hd.wheel[wheel].susCompression;
	out->locked = cp->hd.wheel[wheel].locked;
	out->onGrass = cp->hd.wheel[wheel].onGrass;
	out->surface = cp->hd.wheel[wheel].surface;
	return 1;
}

static int hostCarCosmeticInfo(void* car, JER_COSMETIC_INFO* out)
{
	CAR_DATA* cp = (CAR_DATA*)car;
	CAR_COSMETICS* cos;

	if (cp == NULL || out == NULL || cp->ap.carCos == NULL)
		return 0;

	cos = cp->ap.carCos;
	memset(out, 0, sizeof(*out));
	out->size = sizeof(*out);
	hostSetVec(&out->cog, cos->cog.vx, cos->cog.vy, cos->cog.vz);
	hostSetVec(&out->colBox, cos->colBox.vx, cos->colBox.vy, cos->colBox.vz);
	out->mass = cos->mass;
	out->twistRateX = cos->twistRateX;
	out->twistRateY = cos->twistRateY;
	out->twistRateZ = cos->twistRateZ;
	out->cbYoffset = cos->cbYoffset;
	out->susCoeff = cos->susCoeff;
	out->traction = cos->traction;
	out->wheelSize = cos->wheelSize;
	return 1;
}

static int hostCarSetMotion(void* car, const JER_VEC* position, const JER_QUAT* orientation,
							const JER_VEC* velocity, const JER_VEC* angularVelocity)
{
	CAR_DATA* cp = (CAR_DATA*)car;

	if (cp == NULL)
		return 0;

	if (position != NULL)
	{
		cp->st.n.fposition[0] = position->x;
		cp->st.n.fposition[1] = position->y;
		cp->st.n.fposition[2] = position->z;
	}

	if (orientation != NULL)
	{
		cp->st.n.orientation[0] = orientation->x;
		cp->st.n.orientation[1] = orientation->y;
		cp->st.n.orientation[2] = orientation->z;
		cp->st.n.orientation[3] = orientation->w;
	}

	if (velocity != NULL)
	{
		cp->st.n.linearVelocity[0] = velocity->x;
		cp->st.n.linearVelocity[1] = velocity->y;
		cp->st.n.linearVelocity[2] = velocity->z;
	}

	if (angularVelocity != NULL)
	{
		cp->st.n.angularVelocity[0] = angularVelocity->x;
		cp->st.n.angularVelocity[1] = angularVelocity->y;
		cp->st.n.angularVelocity[2] = angularVelocity->z;
	}

	return 1;
}

static int hostCarSetSpeed(void* car, int speed)
{
	CAR_DATA* cp = (CAR_DATA*)car;

	if (cp == NULL)
		return 0;

	cp->hd.speed = speed;
	return 1;
}

static int hostCarMarkCollision(void* car, int otherId)
{
	CAR_DATA* cp = (CAR_DATA*)car;

	if (cp == NULL || otherId < 0 || otherId >= MAX_CARS)
		return 0;

	cp->hd.mayBeColliding |= (1u << otherId);
	return 1;
}

static int hostCarDrawVertices(void* car, JER_SVEC** out, int* capacity)
{
	CAR_DATA* cp = (CAR_DATA*)car;

	if (cp == NULL || out == NULL || capacity == NULL ||
		cp->id < 0 || cp->id >= MAX_CARS)
		return 0;

	*out = (JER_SVEC*)gTempCarVertDump[cp->id];
	*capacity = MAX_DENTING_VERTS;
	return 1;
}

static const JER_HOST_CAR_API gCar =
{
	sizeof(JER_HOST_CAR_API), JER_HOST_API_VERSION,
	hostCarCount, hostCarAt, hostCarSlotUsable,
	hostCarGetState, hostCarSetState, hostCarWheelMask,
	hostCarModel, hostCarCosmetic, hostCarModelIndex,
	hostCarInit, hostCarMapHeight, hostCarRoadInfo, hostCarCellEmpty,
	hostCarActiveSlots,
	hostCarGetMatrix, hostCarRebuildMatrix, hostCarAddAngularAccel,
	hostCarWheelState, hostCarCosmeticInfo,
	hostCarSetMotion, hostCarSetSpeed, hostCarMarkCollision,
	hostCarDrawVertices
};

/* ------------------------------------------------------------------ */
/* Player / pads                                                       */
/* ------------------------------------------------------------------ */

static int hostPlayerCount(void)
{
	return MAX_PLAYERS;
}

static void* hostPlayerAt(int playerId)
{
	if (playerId < 0 || playerId >= MAX_PLAYERS)
		return NULL;

	return &player[playerId];
}

static int hostPlayerGetState(void* token, JER_PLAYER_STATE* out)
{
	PLAYER* p = (PLAYER*)token;

	if (p == NULL || out == NULL)
		return 0;

	memset(out, 0, sizeof(*out));
	out->size = sizeof(*out);
	out->playerId = (int)(p - player);
	out->playerCarId = p->playerCarId;
	out->cameraCarId = p->cameraCarId;
	out->cameraAngle = p->cameraAngle;
	out->targetCarId = p->targetCarId;
	out->padid = p->padid;
	out->playerType = p->playerType;
	out->horn = p->horn.on ? 1 : 0;
	hostSetVec(&out->position, p->pos[0], p->pos[1], p->pos[2]);
	out->dir = p->dir;
	return 1;
}

static int hostPlayerFelony(void* token)
{
	PLAYER* p = (PLAYER*)token;

	if (p == NULL)
		return 0;

	if (p->playerCarId < 0)
		return pedestrianFelony;

	if (p->playerCarId >= MAX_CARS)
		return 0;

	return car_data[(int)p->playerCarId].felonyRating;
}

static int hostPlayerSetCameraAngle(void* token, int angle){
	PLAYER* p = (PLAYER*)token;

	if (p == NULL)
		return 0;

	p->cameraAngle = angle;
	return 1;
}

static int hostPlayerSetHorn(void* token, int on)
{
	PLAYER* p = (PLAYER*)token;

	if (p == NULL)
		return 0;

	p->horn.on = on ? 1 : 0;
	return 1;
}

static int hostPlayerPad(int playerId, int* out, int max)
{
	if (out == NULL || max <= 0 || playerId < 0 || playerId >= 2)
		return 0;

	out[0] = Pads[playerId].mapped;
	if (max > 1)
		out[1] = Pads[playerId].mapnew;

	return (max > 1) ? 2 : 1;
}

static void* hostPlayerMain(void)
{
	return &MainPlayer;
}

static int hostPlayerPadDirect(int playerId)
{
	if (playerId < 0 || playerId >= 2)
		return 0;

	return Pads[playerId].direct;
}

static const JER_HOST_PLAYER_API gPlayer =
{
	sizeof(JER_HOST_PLAYER_API), JER_HOST_API_VERSION,
	hostPlayerCount, hostPlayerAt, hostPlayerGetState, hostPlayerFelony,
	hostPlayerSetCameraAngle, hostPlayerSetHorn, hostPlayerPad,
	hostPlayerPadDirect, hostPlayerMain
};

/* ------------------------------------------------------------------ */
/* Camera                                                              */
/* ------------------------------------------------------------------ */

static int hostCameraGetState(JER_CAMERA_STATE* out)
{
	if (out == NULL)
		return 0;

	memset(out, 0, sizeof(*out));
	out->size = sizeof(*out);
	out->angle = player[0].cameraAngle;
	out->baseAngle = gCameraAngle;
	out->defaultScrZ = gCameraDefaultScrZ;
	hostSetVec(&out->position, camera_position.vx, camera_position.vy, camera_position.vz);
	return 1;
}

static int hostCameraAngle(void)
{
	return player[0].cameraAngle;
}

static void hostCameraSetAngle(int angle)
{
	player[0].cameraAngle = angle;
	gCameraAngle = angle;
}

static void hostCameraSetBaseAngle(int angle)
{
	gCameraAngle = angle;
}

static const JER_HOST_CAMERA_API gCamera =
{
	sizeof(JER_HOST_CAMERA_API), JER_HOST_API_VERSION,
	hostCameraGetState, hostCameraAngle, hostCameraSetAngle,
	hostCameraSetBaseAngle
};

/* ------------------------------------------------------------------ */
/* Models                                                              */
/* ------------------------------------------------------------------ */

static void* hostModelClean(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return NULL;
	return gCarCleanModelPtr[slot];
}

static void* hostModelDamaged(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return NULL;
	return gCarDamModelPtr[slot];
}

static void* hostModelLow(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return NULL;
	return gCarLowModelPtr[slot];
}

static void* hostModelCosmetic(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return NULL;
	return &car_cosmetics[slot];
}

static const JER_SVEC* hostModelVertices(void* model)
{
	if (model == NULL)
		return NULL;
	return (const JER_SVEC*)_MDL_GETTER_vertices((MODEL*)model);
}

static int hostModelVertexCount(void* model)
{
	if (model == NULL)
		return 0;
	return (int)((MODEL*)model)->num_vertices;
}

static int hostModelCosmeticInfo(int slot, JER_COSMETIC_INFO* out)
{
	CAR_COSMETICS* cos;

	if (out == NULL || slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return 0;

	cos = &car_cosmetics[slot];
	memset(out, 0, sizeof(*out));
	out->size = sizeof(*out);
	hostSetVec(&out->cog, cos->cog.vx, cos->cog.vy, cos->cog.vz);
	hostSetVec(&out->colBox, cos->colBox.vx, cos->colBox.vy, cos->colBox.vz);
	out->mass = cos->mass;
	out->twistRateX = cos->twistRateX;
	out->twistRateY = cos->twistRateY;
	out->twistRateZ = cos->twistRateZ;
	out->cbYoffset = cos->cbYoffset;
	out->susCoeff = cos->susCoeff;
	out->traction = cos->traction;
	out->wheelSize = cos->wheelSize;
	return 1;
}

static const JER_HOST_MODEL_API gModel =
{
	sizeof(JER_HOST_MODEL_API), JER_HOST_API_VERSION,
	hostModelClean, hostModelDamaged, hostModelLow, hostModelCosmetic,
	hostModelVertices, hostModelVertexCount, hostModelCosmeticInfo
};

/* ------------------------------------------------------------------ */
/* World                                                               */
/* ------------------------------------------------------------------ */

static int hostWorldCellsAcross(void)
{
	return cells_across;
}

static int hostWorldUnitsAcrossHalved(void)
{
	return units_across_halved;
}

static int hostWorldNumRoads(int kind)
{
	if (kind == 0)
		return NumDriver2Straights;
	if (kind == 1)
		return NumDriver2Curves;
	if (kind == 2)
		return NumDriver2Junctions;
	return 0;
}

static int hostWorldRegion(void)
{
	return current_region;
}

static int hostWorldRegionOf(int x, int z)
{
	return jer_map_region_of(x, z);
}

static int hostWorldSpoolTo(int x, int z)
{
	return jer_map_spool_to(x, z);
}

static const JER_HOST_WORLD_API gWorld =
{
	sizeof(JER_HOST_WORLD_API), JER_HOST_API_VERSION,
	hostWorldCellsAcross, hostWorldUnitsAcrossHalved, hostWorldNumRoads,
	hostWorldRegion, hostWorldRegionOf, hostWorldSpoolTo
};

/* ------------------------------------------------------------------ */
/* Game                                                                */
/* ------------------------------------------------------------------ */

static int hostGameLevel(void)
{
	return GameLevel;
}

static int hostGameType(void)
{
	return (int)GameType;
}

static int hostGameNumPlayers(void)
{
	return NumPlayers;
}

static int hostGameCopsAllowed(void)
{
	return CopsAllowed;
}

static int hostGameMaxPlayerDamage(void)
{
	return MaxPlayerDamage[0];
}

static int hostGamePedestrianFelony(void)
{
	return pedestrianFelony;
}

static int hostGameFrame(void)
{
	return FrameCnt;
}

static const JER_HOST_GAME_API gGame =
{
	sizeof(JER_HOST_GAME_API), JER_HOST_API_VERSION,
	hostGameLevel, hostGameType, hostGameNumPlayers,
	hostGameCopsAllowed, hostGameMaxPlayerDamage, hostGamePedestrianFelony,
	hostGameFrame
};

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static void hostUiHudMessage(const char* text, int frames)
{
	jer_hud_message(text, frames);
}

static void hostUiHudMessageSegs(const void* segs, int count, int frames)
{
	jer_hud_message_segs((const JER_HUD_SEG*)segs, count, frames);
}

static void hostUiHudClear(void)
{
	jer_hud_clear();
}

static void hostUiNotify(const char* text, int kind, int seconds)
{
	jer_notify(text, kind, seconds);
}

static void hostUiPauseMenu(const void* menu)
{
	jer_pause_menu_register((const JER_PAUSE_MENU*)menu);
}

static void hostUiFrontendMenu(const void* menu)
{
	jer_frontend_register_menu((const JER_FE_MENU*)menu);
}

static void hostUiTextDrawAt(int x, int y, const char* text, int colour, int scale)
{
	jer_text_draw_at(0, scale, x, y, text,
		(colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff);
}

static int hostUiTextWidth(const char* text, int scale)
{
	return jer_text_width(0, scale, text);
}

static int hostUiSoundLock(int channel)
{
	return jer_sound_lock(channel);
}

static void hostUiSoundUnlock(int voice)
{
	jer_sound_unlock(voice);
}

static const JER_HOST_UI_API gUi =
{
	sizeof(JER_HOST_UI_API), JER_HOST_API_VERSION,
	hostUiHudMessage, hostUiHudMessageSegs, hostUiHudClear,
	jer_console_log, hostUiNotify, hostUiPauseMenu, hostUiFrontendMenu,
	hostUiTextDrawAt, hostUiTextWidth, hostUiSoundLock, hostUiSoundUnlock
};

/* ------------------------------------------------------------------ */
/* Debug                                                               */
/* ------------------------------------------------------------------ */

static void hostDebugLine(const JER_VEC* a, const JER_VEC* b, int colour)
{
	VECTOR va = hostVector(a);
	VECTOR vb = hostVector(b);
	CVECTOR c;

	c.r = (unsigned char)((colour >> 16) & 0xff);
	c.g = (unsigned char)((colour >> 8) & 0xff);
	c.b = (unsigned char)(colour & 0xff);
	c.cd = 0;
	Debug_AddLine(va, vb, c);
}

static void hostDebugLineDepth(const JER_VEC* a, const JER_VEC* b, const JER_CVEC* colour)
{
	VECTOR va = hostVector(a);
	VECTOR vb = hostVector(b);
	CVECTOR c = hostCvector(colour);

	Debug_AddLineDepth(va, vb, c);
}

static const JER_HOST_DEBUG_API gDebug =
{
	sizeof(JER_HOST_DEBUG_API), JER_HOST_API_VERSION,
	hostDebugLine, hostDebugLineDepth
};

/* Car data / import / car list                                       */
/* ------------------------------------------------------------------ */

#define HOST_CARLIST_SLOTS 10

static int hostCardataModelSource(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return -1;

	return GetCarModelSourceCity(slot);
}

static void hostCardataSetModelSource(int slot, int city)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return;

	JerSetCarModelSource(slot, city);
}

static int hostCardataResidentModel(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return -1;

	return residentCarModels[slot];
}

static void hostCardataSetResidentModel(int slot, int model)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return;

	residentCarModels[slot] = model;
}

static int hostCardataPaletteCount(int city, int model)
{
	if (city < 0 || city >= CITY_COUNT)
		return 0;

	return CarModelPaletteCount(city, model);
}

static int hostCardataHotLoadModel(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return 0;

	return JerHotLoadCarModel(slot);
}

static int hostCardataHotLoadCosmetics(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return 0;

	return JerHotLoadCarCosmetics(slot);
}

static int hostCardataApplyPalette(int city)
{
	if (city < 0 || city >= CITY_COUNT)
		return 0;

	return CarImportApplyPaletteForCity(city);
}

static int hostCardataReleaseImports(void)
{
	return JerReleaseUnusedCarImports();
}

static int hostCardataSlotResReport(void)
{
	return CarSlotResReport();
}

static void hostCardataInitImport(void)
{
	InitCarImport();
}

static void hostCardataInitImportMidLevel(void)
{
	InitCarImportMidLevel();
}

static int hostCardataAvailability(int city, int slot)
{
	if (city < 0 || city >= CITY_COUNT || slot < 0 || slot >= HOST_CARLIST_SLOTS)
		return 0;

	return CarAvailability[city][slot];
}

static void hostCardataSetAvailability(int city, int slot, int available)
{
	if (city < 0 || city >= CITY_COUNT || slot < 0 || slot >= HOST_CARLIST_SLOTS)
		return;

	CarAvailability[city][slot] = available != 0;
}

static int hostCardataListModel(int city, int slot)
{
	if (city < 0 || city >= CITY_COUNT || slot < 0 || slot >= HOST_CARLIST_SLOTS)
		return 0;

	return (int)(signed char)carNumLookup[city][slot];
}

static void hostCardataSetListModel(int city, int slot, int model)
{
	if (city < 0 || city >= CITY_COUNT || slot < 0 || slot >= HOST_CARLIST_SLOTS)
		return;

	carNumLookup[city][slot] = (char)model;
}

static const JER_HOST_CARDATA_API gCardata =
{
	sizeof(JER_HOST_CARDATA_API), JER_HOST_API_VERSION,
	hostCardataModelSource, hostCardataSetModelSource,
	hostCardataResidentModel, hostCardataSetResidentModel,
	hostCardataPaletteCount, hostCardataHotLoadModel,
	hostCardataHotLoadCosmetics, hostCardataApplyPalette,
	hostCardataReleaseImports, hostCardataSlotResReport,
	hostCardataInitImport, hostCardataInitImportMidLevel,
	hostCardataAvailability, hostCardataSetAvailability,
	hostCardataListModel, hostCardataSetListModel
};

/* ------------------------------------------------------------------ */
/* The root table and the one exported symbol                          */
/* ------------------------------------------------------------------ */

static const JER_HOST_API gHostApi =
{
	sizeof(JER_HOST_API), JER_HOST_API_VERSION, JER_HOST_ABI_V1,
	&gCore, &gConfig, &gCar, &gPlayer, &gCamera, &gModel,
	&gWorld, &gGame, &gUi, &gDebug, &gCardata
};

/* Keep the resolver alive through /OPT:REF and put it in the linker map, so
 * the two-pass exports.def exports it. */
void* volatile gJerHostApiKeepalive = (void*)&jer_host_api;

#ifdef __cplusplus
extern "C" {
#endif

const JER_HOST_API* jer_host_api(void)
{
	return &gHostApi;
}

#ifdef __cplusplus
}
#endif
