/*
 * jer_host_helpers.h -- small inline wrappers around JER_HOST_API.
 *
 * An external addon uses these so the common "resolve the table, get a car
 * state, read the clean model, ask for the draw buffer" sequence is one call
 * instead of repeated null checks. The wrappers do not add ABI surface; they
 * are inline over jerHostApiGet()/JER_HOST_API.
 */

#ifndef JER_HOST_HELPERS_H
#define JER_HOST_HELPERS_H

#include "jer_host_api.h"

#ifdef __cplusplus
extern "C" {
#endif

JER_HOST_INLINE const JER_HOST_API* jerHost(void)
{
	return jerHostApiGet();
}

JER_HOST_INLINE int jerHostCarState(void* car, JER_CAR_STATE* out)
{
	const JER_HOST_API* api = jerHost();
	return (api != NULL && api->car != NULL && api->car->get_state != NULL)
		? api->car->get_state(car, out) : 0;
}

JER_HOST_INLINE int jerHostCarModelSlot(void* car)
{
	const JER_HOST_API* api = jerHost();
	return (api != NULL && api->car != NULL && api->car->model_index != NULL)
		? api->car->model_index(car) : -1;
}

JER_HOST_INLINE void* jerHostCarModel(void* car)
{
	const JER_HOST_API* api = jerHost();
	return (api != NULL && api->car != NULL && api->car->model != NULL)
		? api->car->model(car) : 0;
}

JER_HOST_INLINE int jerHostCarDrawVertices(void* car, JER_SVEC** out, int* capacity)
{
	const JER_HOST_API* api = jerHost();
	return (api != NULL && api->car != NULL && api->car->draw_vertices != NULL)
		? api->car->draw_vertices(car, out, capacity) : 0;
}

JER_HOST_INLINE int jerHostCarCosmetic(void* car, JER_COSMETIC_INFO* out)
{
	const JER_HOST_API* api = jerHost();
	return (api != NULL && api->car != NULL && api->car->cosmetic_info != NULL)
		? api->car->cosmetic_info(car, out) : 0;
}

JER_HOST_INLINE const JER_SVEC* jerHostModelVertices(void* model)
{
	const JER_HOST_API* api = jerHost();
	return (api != NULL && api->model != NULL && api->model->vertices != NULL)
		? api->model->vertices(model) : 0;
}

JER_HOST_INLINE int jerHostModelVertexCount(void* model)
{
	const JER_HOST_API* api = jerHost();
	return (api != NULL && api->model != NULL && api->model->vertex_count != NULL)
		? api->model->vertex_count(model) : 0;
}

JER_HOST_INLINE int jerHostConfigGetInt(const char* mod, const char* key, int def)
{
	const JER_HOST_API* api = jerHost();
	return (api != NULL && api->config != NULL && api->config->get_int != NULL)
		? api->config->get_int(mod, key, def) : def;
}

JER_HOST_INLINE void jerHostConfigSetInt(const char* mod, const char* key, int value)
{
	const JER_HOST_API* api = jerHost();
	if (api != NULL && api->config != NULL && api->config->set_int != NULL)
		api->config->set_int(mod, key, value);
}

JER_HOST_INLINE int jerHostFrame(void)
{
	const JER_HOST_API* api = jerHost();
	return (api != NULL && api->game != NULL && api->game->frame != NULL)
		? api->game->frame() : 0;
}

JER_HOST_INLINE int jerHostPadDirect(int playerId)
{
	const JER_HOST_API* api = jerHost();
	return (api != NULL && api->player != NULL && api->player->pad_direct != NULL)
		? api->player->pad_direct(playerId) : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* JER_HOST_HELPERS_H */
