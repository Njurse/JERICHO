#ifndef NATTYDEMENU_H
#define NATTYDEMENU_H

#include "pause.h"   /* must be here */

/* function prototypes – note the semicolons */
void NattyDOption1(int direction);
void NattyDOption2(int direction);

/* extern declarations – must end with semicolon */
extern MENU_ITEM NattyDItems[];
extern MENU_HEADER NattyDMenuHeader;

#endif