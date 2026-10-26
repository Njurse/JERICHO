#ifndef JERICHO_JER_HOST_API_H
#define JERICHO_JER_HOST_API_H

/*
 * jer_host_api.h -- the stable JERICHO host API (interface 3 / API v1).
 *
 * One exported resolver returns a versioned table of function pointers. An
 * addon resolves it at runtime (GetProcAddress / dlsym), so it links no game
 * symbols and works in JERICHO.exe and JERICHO_dev.exe alike. The table is the
 * only thing an addon may rely on: engine structs are never exposed here.
 *
 * Versioning is append-only. `size` lets an old addon use the prefix it knows;
 * a new function goes at the end of a subtable and the host publishes the new
 * size. `abi` is the build/layout contract for the views in this header.
 */

#include <stddef.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define JER_HOST_API_VERSION 1u
/* "JH01": bump whenever a view/field this header defines changes. */
#define JER_HOST_ABI_V1 0x4A483031u

typedef struct JER_VEC
{
	int x, y, z, pad;
} JER_VEC;

typedef struct JER_SVEC
{
	short x, y, z, pad;
} JER_SVEC;

typedef struct JER_QUAT
{
	int x, y, z, w;
} JER_QUAT;

typedef struct JER_CVEC
{
	unsigned char r, g, b, code;
} JER_CVEC;

/* A car as the host sees it. `size` is sizeof(JER_CAR_STATE) at the caller's
 * build; the host fills the prefix it knows. */
typedef struct JER_CAR_STATE
{
	unsigned int size;
	int carId;
	JER_VEC position;			/* world position, fixed point */
	JER_QUAT orientation;			/* body quaternion */
	JER_VEC velocity;			/* linear velocity */
	JER_VEC angularVelocity;		/* angular velocity */
	int speed;				/* hd.speed */
	int wheelSpeed;				/* hd.wheel_speed */
	int wheelMask;				/* 1 bit per wheel on the ground */
	int controlType;			/* CONTROL_TYPE_* */
	int controlFlags;			/* CONTROL_FLAG_* */
	int model;				/* ap.model */
	int palette;				/* ap.palette */
	int damage;				/* totalDamage */
	int playerId;				/* owning player, -1 if none */
	JER_SVEC wheelAngle;			/* front wheel steering angle */
	int thrust;
	int revs;
	int gear;
} JER_CAR_STATE;

typedef struct JER_PLAYER_STATE
{
	unsigned int size;
	int playerId;
	int playerCarId;
	int cameraCarId;
	int cameraAngle;
	int targetCarId;
	int padid;
	int playerType;				/* PLAYER_TYPE_* */
	int horn;				/* 1 while the horn is held */
	JER_VEC position;
	int dir;
} JER_PLAYER_STATE;

typedef struct JER_CAMERA_STATE
{
	unsigned int size;
	int angle;				/* PLAYER.cameraAngle */
	int baseAngle;				/* gCameraAngle */
	int defaultScrZ;			/* gCameraDefaultScrZ */
	JER_VEC position;			/* camera_position */
} JER_CAMERA_STATE;

typedef struct JER_HOST_ROAD_INFO
{
	int surfId;				/* < 0 = no surface here */
	int kind;				/* 0 straight, 1 curve, 2 junction */
	int numLanes;
	int speedLimit;
	unsigned char laneDirs;
	unsigned char aiLanes;
	int connect[4];
} JER_HOST_ROAD_INFO;

/* ------------------------------------------------------------------ */
/* Subtables                                                          */
/* ------------------------------------------------------------------ */

typedef struct JER_HOST_CORE_API
{
	unsigned int size;
	unsigned int version;
	void (*log)(const char* fmt, ...);
	int  (*fire)(int event, void* args);
	void (*override)(int slot, void* fn);
} JER_HOST_CORE_API;

typedef struct JER_HOST_CONFIG_API
{
	unsigned int size;
	unsigned int version;
	int  (*get_int)(const char* mod, const char* key, int def);
	void (*set_int)(const char* mod, const char* key, int value);
	int  (*get_bool)(const char* mod, const char* key, int def);
	void (*set_bool)(const char* mod, const char* key, int value);
	const char* (*get_str)(const char* mod, const char* key, const char* def);
	void (*set_str)(const char* mod, const char* key, const char* value);
} JER_HOST_CONFIG_API;

typedef struct JER_HOST_CAR_API
{
	unsigned int size;
	unsigned int version;
	int  (*count)(void);
	void* (*at)(int slot);				/* opaque CAR_DATA token */
	int  (*slot_usable)(int slot);
	int  (*get_state)(void* car, JER_CAR_STATE* out);
	int  (*set_state)(void* car, const JER_CAR_STATE* in);
	int  (*wheel_mask)(void* car);
	void* (*model)(void* car);			/* clean MODEL* */
	void* (*cosmetic)(void* car);			/* CAR_COSMETICS* */
	int  (*model_index)(void* car);
	int  (*init)(int slot, int model, const JER_VEC* position, const JER_SVEC* orientation);
	int  (*map_height)(const JER_VEC* position);
	int  (*road_info)(int x, int y, int z, JER_HOST_ROAD_INFO* out);
	int  (*cell_empty)(const JER_VEC* position, int flags);
	int  (*active_slots)(int* out, int max);
} JER_HOST_CAR_API;

typedef struct JER_HOST_PLAYER_API
{
	unsigned int size;
	unsigned int version;
	int  (*count)(void);
	void* (*at)(int playerId);			/* opaque PLAYER token */
	int  (*get_state)(void* player, JER_PLAYER_STATE* out);
	int  (*set_camera_angle)(void* player, int angle);
	int  (*set_horn)(void* player, int on);
	int  (*pad)(int playerId, int* out, int max);	/* raw PAD words */
} JER_HOST_PLAYER_API;

typedef struct JER_HOST_CAMERA_API
{
	unsigned int size;
	unsigned int version;
	int  (*get_state)(JER_CAMERA_STATE* out);
	int  (*angle)(void);
	void (*set_angle)(int angle);
} JER_HOST_CAMERA_API;

typedef struct JER_HOST_MODEL_API
{
	unsigned int size;
	unsigned int version;
	void* (*clean)(int slot);			/* MODEL* */
	void* (*damaged)(int slot);			/* MODEL* */
	void* (*low)(int slot);				/* MODEL* */
	void* (*cosmetic)(int slot);			/* CAR_COSMETICS* */
	const JER_SVEC* (*vertices)(void* model);	/* model vertex array (SVECTOR layout) */
	int  (*vertex_count)(void* model);
} JER_HOST_MODEL_API;

typedef struct JER_HOST_WORLD_API
{
	unsigned int size;
	unsigned int version;
	int  (*cells_across)(void);
	int  (*units_across_halved)(void);
	int  (*num_roads)(int kind);			/* 0 straight, 1 curve, 2 junction */
	int  (*region)(void);
	int  (*region_of)(int x, int z);
	int  (*spool_to)(int x, int z);
} JER_HOST_WORLD_API;

typedef struct JER_HOST_GAME_API
{
	unsigned int size;
	unsigned int version;
	int  (*level)(void);
	int  (*game_type)(void);
	int  (*num_players)(void);
	int  (*cops_allowed)(void);
	int  (*max_player_damage)(void);
	int  (*pedestrian_felony)(void);
} JER_HOST_GAME_API;

typedef struct JER_HOST_UI_API
{
	unsigned int size;
	unsigned int version;
	void (*hud_message)(const char* text, int frames);
	void (*hud_message_segs)(const void* segs, int count, int frames);
	void (*hud_clear)(void);
	void (*console_log)(const char* fmt, ...);
	void (*notify)(const char* text, int kind, int seconds);
	void (*pause_menu_register)(const void* menu);
	void (*frontend_register_menu)(const void* menu);
	void (*text_draw_at)(int x, int y, const char* text, int colour, int scale);
	int  (*text_width)(const char* text, int scale);
	int  (*sound_lock)(int channel);
	void (*sound_unlock)(int voice);
} JER_HOST_UI_API;

typedef struct JER_HOST_DEBUG_API
{
	unsigned int size;
	unsigned int version;
	void (*line)(const JER_VEC* a, const JER_VEC* b, int colour);
	void (*line_depth)(const JER_VEC* a, const JER_VEC* b, const JER_CVEC* colour);
} JER_HOST_DEBUG_API;

typedef struct JER_HOST_API
{
	unsigned int size;
	unsigned int version;
	unsigned int abi;
	const JER_HOST_CORE_API* core;
	const JER_HOST_CONFIG_API* config;
	const JER_HOST_CAR_API* car;
	const JER_HOST_PLAYER_API* player;
	const JER_HOST_CAMERA_API* camera;
	const JER_HOST_MODEL_API* model;
	const JER_HOST_WORLD_API* world;
	const JER_HOST_GAME_API* game;
	const JER_HOST_UI_API* ui;
	const JER_HOST_DEBUG_API* debug;
} JER_HOST_API;

/* The host implementation. This is the only symbol an addon needs. */
const JER_HOST_API* jer_host_api(void);

/* Resolve the table at runtime. The addon links no game symbols, so this works
 * in any exe name/config and on both platforms. */
#if defined(_MSC_VER)
#define JER_HOST_INLINE static __inline
#else
#define JER_HOST_INLINE static inline
#endif

JER_HOST_INLINE const JER_HOST_API* jerHostApiGet(void)
{
	static const JER_HOST_API* cached = 0;

	if (cached == 0)
	{
		typedef const JER_HOST_API* (*JER_HOST_API_FN)(void);
		JER_HOST_API_FN fn = 0;

#if defined(_WIN32)
		HMODULE host = GetModuleHandleA(0);

		if (host != 0)
			fn = (JER_HOST_API_FN)(void*)GetProcAddress(host, "jer_host_api");
#else
		fn = (JER_HOST_API_FN)dlsym(RTLD_DEFAULT, "jer_host_api");
#endif
		if (fn != 0)
			cached = fn();
	}

	if (cached == 0 || cached->size < sizeof(JER_HOST_API) ||
		cached->version != JER_HOST_API_VERSION || cached->abi != JER_HOST_ABI_V1)
		return 0;

	return cached;
}

#ifdef __cplusplus
}
#endif

#endif /* JERICHO_JER_HOST_API_H */
