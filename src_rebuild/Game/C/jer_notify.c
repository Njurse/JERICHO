// jer_notify.c — jer_notify (JERICHO/include/jer_notify.h).
//
// Engine-side, like jer_hud.c: the notice is the game's own (SetPlayerMessage ->
// DrawMessage), and only the game can queue one. Modules just call jer_notify.

#include "driver2.h"
#include "mission.h"		/* SetPlayerMessage, Mission */

#include "JERICHO/include/jer_notify.h"

#include <string.h>

// The engine keeps the POINTER it is handed (Mission.message_string[]), so a
// call has to hand it storage that outlives the call. This is that storage.
static char sNotifyText[JER_NOTIFY_TEXT_MAX];

int jer_notify(const char* text, int priority, int seconds)
{
	int n;

	if (text == NULL || text[0] == '\0')
		return 0;

	if (seconds <= 0)
		seconds = 3;
	if (seconds > JER_NOTIFY_SECONDS_MAX)
		seconds = JER_NOTIFY_SECONDS_MAX;

	if (priority < 0)
		priority = 0;
	if (priority > JER_NOTIFY_PRIORITY_MAX)
		priority = JER_NOTIFY_PRIORITY_MAX;

	for (n = 0; n < JER_NOTIFY_TEXT_MAX - 1 && text[n] != '\0'; n++)
		sNotifyText[n] = text[n];

	sNotifyText[n] = '\0';

	SetPlayerMessage(0, sNotifyText, priority, seconds);
	return 1;
}

void jer_notify_clear(void)
{
	Mission.message_timer[0] = 0;
	Mission.message_string[0] = NULL;
}
