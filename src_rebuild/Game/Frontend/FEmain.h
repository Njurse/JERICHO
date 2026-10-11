#ifndef FEMAIN_H
#define FEMAIN_H

extern int gInFrontend;
extern int bRedrawFrontend;

extern void InitFrontendDisplay();
extern void InitFrontend();

extern void State_FrontEnd(void* param); // 0x001C296C

extern void SetFEDrawMode();
extern void LoadFrontendScreens(int full);
extern void ReInitScreens(int returnToMain);

/* JERICHO-HOOK: put the frontend primitive buffer + ordering table back to the
 * state EndFrame() expects. Anything painting a frame WITHOUT going through
 * EndFrame must call this - the boot screens do it by hand, and the vanilla
 * save-wait message did not, which left the OT pointing at the frame that was
 * in flight (an access violation in the ordering-table walk). */
extern void JerichoResetFEPrimState(void);

#endif
