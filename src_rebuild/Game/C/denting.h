#ifndef DENTING_H
#define DENTING_H

/* A drawn vertex this far from its clean position is corruption, not a dent.
 * Shared by DentCarDirectional's per-vertex clamp (denting.c) and the
 * STRETCHED VERTEX canary (cars.c) so the two cannot drift apart. The per-car
 * crush multiplier can legitimately move bodywork ~1.3k units at 10x, so 4096
 * leaves headroom while still catching a wild write. */
#define JER_STRETCH_VERT	4096


extern void InitialiseDenting(); // 0x0003B1C0
extern void LoadDenting(int level); // 0x0003B1F0

extern void DentCar(CAR_DATA *cp); // 0x0003A310
extern void CreateDentableCar(CAR_DATA *cp); // 0x0003A6E4

extern void InitHubcap(); // 0x0003A874
extern void HandlePlayerHubcaps(int playerId);
extern void MoveHubcap(); // 0x0003AB4C

extern void SetupSpecDenting(char *loadbuffer); // 0x0003B240
extern void DentCar(CAR_DATA* cp);
extern void DentCarDirectional(CAR_DATA* cp, VECTOR collisionpoint);
#endif
