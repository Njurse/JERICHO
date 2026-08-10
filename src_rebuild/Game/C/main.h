#ifndef MAIN_H
#define MAIN_H

extern int game_over;
extern int gDemoLevel;
// Set when the current level is a Driver 1 city (GameLevel 4-7). The engine
// gates the D1 loading paths (lump counts, pallets, spooling, cell iteration,
// surface queries, per-city table guards) on this; it is set in
// State_GameStart from (GameLevel >= 4) and is 0 for every Driver 2 level.
extern int gDriver1Level;
// Set to 0 to skip the boot splash screens and intro FMV (straight to menu).
extern int gPlayBootFMV;

extern XZPAIR gStartPos;

extern int xa_timeout;
extern int FrameCnt;
extern int CurrentPlayerView;
extern char CameraChanged;
extern int FrAng;
extern int gStopPadReads;
extern int num_active_cars;
extern int numInactiveCars;

extern int gLightsOn;
extern int NightAmbient;
extern int wetness;

extern int scr_z;

extern int DawnCount;
extern int ObjectDrawnValue;

extern int Havana3DLevelDraw;

extern void SsSetSerialVol(short s_num, short voll, short volr);		// TEMPORARY

extern void State_GameInit(void* param); // 0x00059330
extern void State_GameLoop(void* param); // 0x0005A8DC

extern int FilterFrameTime();

extern void DrawGame(); // 0x0005C458

extern void EndGame(GAMEMODE mode); // 0x0005C574
extern void EnablePause(PAUSEMODE mode); // 0x0005C590

extern int redriver2_main(int argc, char** argv); // 0x0005B384

extern void FadeScreen(int end_value); // 0x0005C668

extern void UpdatePlayerInformation(); // 0x0005B54C

extern void RenderGame2(int view); // 0x0005B888
extern void RenderGame(); // 0x0005C6E0

extern void InitGameVariables(); // 0x0005BCE4

extern void DealWithHorn(char *hr, int i); // 0x0005BF74

typedef void(*occlFunc)(int* comp_val);
extern int Havana3DOcclusion(occlFunc func, int *param); // 0x0005C16C

#endif
