#ifndef COSMETIC_H
#define COSMETIC_H

extern CAR_COSMETICS car_cosmetics[MAX_CAR_RESIDENT_MODELS];
extern CAR_COSMETICS dummyCosmetics;

// LEVELS\<CITY>.LCF - the per-city car colours an imported vehicle must take
// its paint from (models.c reads the matching one)
extern char* CosmeticFiles[];

extern int gcar_num;

extern void LoadCosmetics(int level); // 0x00031160
extern void SetupSpecCosmetics(char *loadbuffer); // 0x00031360

extern void AddReverseLight(CAR_DATA* cp); // 0x0002F994
extern void AddIndicatorLight(CAR_DATA *cp, int Type); // 0x0002FAEC
extern void AddBrakeLight(CAR_DATA *cp); // 0x0002FDE4
extern void AddCopCarLight(CAR_DATA *cp); // 0x00030148
extern void AddNightLights(CAR_DATA *cp); // 0x00030544

extern void AddSmokingEngine(CAR_DATA *cp, int black_smoke, int WheelSpeed); // 0x00030D9C
extern void AddExhaustSmoke(CAR_DATA *cp, int black_smoke, int WheelSpeed);
extern void AddFlamingEngine(CAR_DATA *cp); // 0x00030FAC

/* JERICHO: the same two emitters with the smoke TYPE and the sizes as arguments,
 * so a module's damage ladder can ask for grey or for a bigger fire without a
 * second copy of the emitter. The stock entry points above are these with the
 * original numbers. */
extern void AddSmokingEngineTyped(CAR_DATA *cp, int smokeType, int startW, int endW, int black_offset, int WheelSpeed);
extern void AddFlamingEngineSized(CAR_DATA *cp, int startW, int endW);


#endif
