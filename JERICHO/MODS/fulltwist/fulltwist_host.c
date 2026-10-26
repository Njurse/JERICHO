/*
 * fulltwist_host.c -- the only place FULL TWIST crosses into the game.
 *
 * Every event handler loads a local CAR_DATA view, the physics code works on
 * that view, and the handler flushes the fields the module may change through
 * the host API. No engine header and no engine data global is touched.
 */

#include "jericho.h"
#include "jer_events.h"
#include "jer_host_api.h"
#include "fulltwist_host.h"

static const JER_HOST_API* gHost;

const JER_HOST_API* ftHost(void)
{
	if (gHost == NULL)
		gHost = jerHostApiGet();

	return gHost;
}

static void setVec(JER_VEC* out, int x, int y, int z)
{
	out->x = x;
	out->y = y;
	out->z = z;
	out->pad = 0;
}

void ftCarLoad(const JER_HOST_API* api, void* token, CAR_DATA* out)
{
	JER_CAR_STATE state;
	JER_MATRIX matrix;
	JER_COSMETIC_INFO cos;
	int i;

	if (out == NULL)
		return;

	memset(out, 0, sizeof(*out));
	out->token = token;
	out->padid = -1;

	if (api == NULL || token == NULL || api->car == NULL)
		return;

	if (!api->car->get_state(token, &state))
		return;

	out->id = state.carId;
	out->controlType = state.controlType;
	out->controlFlags = state.controlFlags;
	out->totalDamage = state.damage;
	out->thrust = state.thrust;
	out->wheel_angle = state.wheelAngle.x;
	out->ap.model = state.model;
	out->ap.palette = state.palette;
	out->hd.speed = state.speed;
	out->hd.wheel_speed = state.wheelSpeed;
	out->hd.direction = 0;

	out->st.n.fposition[0] = state.position.x;
	out->st.n.fposition[1] = state.position.y;
	out->st.n.fposition[2] = state.position.z;
	out->st.n.orientation[0] = state.orientation.x;
	out->st.n.orientation[1] = state.orientation.y;
	out->st.n.orientation[2] = state.orientation.z;
	out->st.n.orientation[3] = state.orientation.w;
	out->st.n.linearVelocity[0] = state.velocity.x;
	out->st.n.linearVelocity[1] = state.velocity.y;
	out->st.n.linearVelocity[2] = state.velocity.z;
	out->st.n.angularVelocity[0] = state.angularVelocity.x;
	out->st.n.angularVelocity[1] = state.angularVelocity.y;
	out->st.n.angularVelocity[2] = state.angularVelocity.z;

	if (api->car->get_matrix != NULL && api->car->get_matrix(token, &matrix))
	{
		for (i = 0; i < 3; i++)
		{
			out->hd.where.m[i][0] = matrix.m[i][0];
			out->hd.where.m[i][1] = matrix.m[i][1];
			out->hd.where.m[i][2] = matrix.m[i][2];
		}

		out->hd.where.t[0] = matrix.t.x;
		out->hd.where.t[1] = matrix.t.y;
		out->hd.where.t[2] = matrix.t.z;
	}

	if (api->car->wheel_state != NULL)
	{
		for (i = 0; i < 4; i++)
		{
			JER_WHEEL_STATE wheel;

			if (api->car->wheel_state(token, i, &wheel))
			{
				out->hd.wheel[i].susCompression = wheel.compression;
				out->hd.wheel[i].locked = wheel.locked;
				out->hd.wheel[i].onGrass = wheel.onGrass;
				out->hd.wheel[i].surface = wheel.surface;
			}
		}
	}

	if (api->car->cosmetic_info != NULL && api->car->cosmetic_info(token, &cos))
	{
		out->ap.cos.cog.vx = (short)cos.cog.x;
		out->ap.cos.cog.vy = (short)cos.cog.y;
		out->ap.cos.cog.vz = (short)cos.cog.z;
		out->ap.cos.colBox.vx = (short)cos.colBox.x;
		out->ap.cos.colBox.vy = (short)cos.colBox.y;
		out->ap.cos.colBox.vz = (short)cos.colBox.z;
		out->ap.cos.mass = cos.mass;
		out->ap.cos.twistRateX = cos.twistRateX;
		out->ap.cos.twistRateY = cos.twistRateY;
		out->ap.cos.twistRateZ = cos.twistRateZ;
		out->ap.cos.cbYoffset = cos.cbYoffset;
		out->ap.cos.susCoeff = cos.susCoeff;
		out->ap.cos.traction = cos.traction;
		out->ap.cos.wheelSize = cos.wheelSize;
		out->ap.carCos = &out->ap.cos;
	}
}

void ftCarFlush(const JER_HOST_API* api, void* token, const CAR_DATA* in)
{
	JER_VEC position;
	JER_VEC velocity;
	JER_VEC angular;
	JER_QUAT orientation;
	int i;

	if (api == NULL || token == NULL || api->car == NULL || in == NULL)
		return;

	if (api->car->set_motion != NULL)
	{
		setVec(&position, in->st.n.fposition[0], in->st.n.fposition[1], in->st.n.fposition[2]);
		setVec(&velocity, in->st.n.linearVelocity[0], in->st.n.linearVelocity[1], in->st.n.linearVelocity[2]);
		setVec(&angular, in->st.n.angularVelocity[0], in->st.n.angularVelocity[1], in->st.n.angularVelocity[2]);
		orientation.x = in->st.n.orientation[0];
		orientation.y = in->st.n.orientation[1];
		orientation.z = in->st.n.orientation[2];
		orientation.w = in->st.n.orientation[3];
		api->car->set_motion(token, &position, &orientation, &velocity, &angular);
	}

	if (api->car->set_speed != NULL)
		api->car->set_speed(token, in->hd.speed);

	if (api->car->add_angular_accel != NULL)
	{
		JER_VEC aacc;

		setVec(&aacc, in->hd.aacc[0], in->hd.aacc[1], in->hd.aacc[2]);
		api->car->add_angular_accel(token, &aacc);
	}

	if (api->car->mark_collision != NULL)
	{
		for (i = 0; i < MAX_CARS; i++)
		{
			if (in->hd.mayBeColliding & (1u << i))
				api->car->mark_collision(token, i);
		}
	}
}

void ftCarLoadSlot(const JER_HOST_API* api, int slot, CAR_DATA* out, void** tokenOut)
{
	void* token = NULL;

	if (tokenOut != NULL)
		*tokenOut = NULL;

	if (api == NULL || api->car == NULL || slot < 0 || slot >= MAX_CARS)
		return;

	token = api->car->at(slot);
	if (token == NULL)
		return;

	if (tokenOut != NULL)
		*tokenOut = token;

	ftCarLoad(api, token, out);
}

void ftCarFlushSlot(const JER_HOST_API* api, int slot, const CAR_DATA* in)
{
	void* token;

	if (api == NULL || api->car == NULL || slot < 0 || slot >= MAX_CARS)
		return;

	token = api->car->at(slot);
	if (token != NULL)
		ftCarFlush(api, token, in);
}

int ftCarTokenId(const JER_HOST_API* api, void* token)
{
	JER_CAR_STATE state;

	if (api == NULL || api->car == NULL || token == NULL || !api->car->get_state(token, &state))
		return -1;

	return state.carId;
}

int ftCarSlotUsable(const JER_HOST_API* api, int slot)
{
	return (api != NULL && api->car != NULL) ? api->car->slot_usable(slot) : 0;
}

void* ftMainPlayer(const JER_HOST_API* api)
{
	return (api != NULL && api->player != NULL && api->player->main != NULL) ? api->player->main() : NULL;
}

int ftMainPlayerState(const JER_HOST_API* api, JER_PLAYER_STATE* out)
{
	void* mainPlayer;

	if (api == NULL || api->player == NULL || out == NULL)
		return 0;

	mainPlayer = ftMainPlayer(api);
	if (mainPlayer == NULL)
		return 0;

	return api->player->get_state(mainPlayer, out);
}

void ftCarRebuild(const JER_HOST_API* api, CAR_DATA* car)
{
	if (api == NULL || car == NULL || car->token == NULL)
		return;

	ftCarFlush(api, car->token, car);

	if (api->car != NULL && api->car->rebuild_matrix != NULL)
		api->car->rebuild_matrix(car->token);
}
