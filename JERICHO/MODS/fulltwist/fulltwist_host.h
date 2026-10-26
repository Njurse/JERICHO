/*
 * fulltwist_host.h -- host-API load/flush for the local FT_CAR view.
 */

#ifndef FULLTWIST_HOST_H
#define FULLTWIST_HOST_H

#include "jer_host_api.h"
#include "ft_types.h"

const JER_HOST_API* ftHost(void);

void ftCarLoad(const JER_HOST_API* api, void* token, CAR_DATA* out);
void ftCarFlush(const JER_HOST_API* api, void* token, const CAR_DATA* in);

void ftCarLoadSlot(const JER_HOST_API* api, int slot, CAR_DATA* out, void** tokenOut);
void ftCarFlushSlot(const JER_HOST_API* api, int slot, const CAR_DATA* in);

int ftCarTokenId(const JER_HOST_API* api, void* token);
int ftCarSlotUsable(const JER_HOST_API* api, int slot);
void* ftMainPlayer(const JER_HOST_API* api);
int ftMainPlayerState(const JER_HOST_API* api, JER_PLAYER_STATE* out);
void ftCarRebuild(const JER_HOST_API* api, CAR_DATA* car);

#endif /* FULLTWIST_HOST_H */
