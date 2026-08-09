/* jer_menu.c — the JERICHO frontend-menu library implementation.
 * See jer_menu.h. */

#include "jer_menu.h"
#include "jer_internal.h"

void jer_menu_begin(JerMenu* m, const char* const* items, int count)
{
	if (m == NULL)
		return;

	m->items = items;
	m->count = count > 0 ? count : 0;
	m->cursor = 0;
	m->visible = 1;
}

void jer_menu_end(JerMenu* m)
{
	if (m != NULL)
		m->visible = 0;
}

int jer_menu_update(JerMenu* m, int upBit, int downBit, int selectBit, int curPad, int curPadNew)
{
	if (m == NULL || !m->visible || m->count <= 0)
		return 0;

	if ((curPadNew & upBit) != 0)
	{
		m->cursor--;

		if (m->cursor < 0)
			m->cursor = m->count - 1;
	}
	else if ((curPadNew & downBit) != 0)
	{
		m->cursor++;

		if (m->cursor >= m->count)
			m->cursor = 0;
	}

	return ((curPadNew & selectBit) != 0) ? 1 : 0;
}

const char* jer_menu_label(const JerMenu* m)
{
	if (m == NULL || !m->visible || m->items == NULL ||
		m->cursor < 0 || m->cursor >= m->count)
		return "";

	return m->items[m->cursor];
}
