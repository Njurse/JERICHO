/* jer_menu.c — the JERICHO frontend-menu library implementation.
 * See jer_menu.h. The core stays free of the game headers: the typed menus are
 * pure cursor state, and jer_menu_draw (engine-side) owns the look. */

#include "jer_menu.h"

#include <stddef.h>		/* NULL */

void jer_menu_begin(JerMenu* m, const char* const* items, int count)
{
	if (m == NULL)
		return;

	m->items = items;
	m->count = count > 0 ? count : 0;
	m->cursor = 0;
	m->visible = 1;
	m->kind = JER_MENU_KIND_LIST;
	m->title = NULL;
}

void jer_menu_end(JerMenu* m)
{
	if (m != NULL)
		m->visible = 0;
}

void jer_menu_list(JerMenu* m, const char* title, const char* const* items, int count)
{
	jer_menu_begin(m, items, count);

	if (m != NULL)
	{
		m->kind = JER_MENU_KIND_LIST;
		m->title = title;
	}
}

void jer_menu_yesno(JerMenu* m, const char* title)
{
	if (m == NULL)
		return;

	jer_menu_begin(m, NULL, 2);	/* Yes = 0, No = 1 */

	m->kind = JER_MENU_KIND_YESNO;
	m->title = title;
}

void jer_menu_carousel(JerMenu* m, const char* title, const char* const* items, int count)
{
	jer_menu_begin(m, items, count);

	if (m != NULL)
	{
		m->kind = JER_MENU_KIND_CAROUSEL;
		m->title = title;
	}
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

int jer_menu_tick(JerMenu* m, int upBit, int downBit, int leftBit, int rightBit,
		  int selectBit, int curPad, int curPadNew)
{
	int moved = 0;

	(void)curPad;

	if (m == NULL || !m->visible || m->count <= 0)
		return 0;

	/* YESNO / CAROUSEL move left/right (falling back to up/down when the caller
	 * only passes those); a LIST moves up/down. */
	if (m->kind == JER_MENU_KIND_YESNO || m->kind == JER_MENU_KIND_CAROUSEL)
	{
		int lb = (leftBit != 0) ? leftBit : upBit;
		int rb = (rightBit != 0) ? rightBit : downBit;

		if (lb != 0 && (curPadNew & lb) != 0)
		{
			m->cursor--;
			moved = 1;
		}
		else if (rb != 0 && (curPadNew & rb) != 0)
		{
			m->cursor++;
			moved = 1;
		}
	}

	if (!moved && m->kind == JER_MENU_KIND_LIST)
	{
		if ((curPadNew & upBit) != 0)
		{
			m->cursor--;
			moved = 1;
		}
		else if ((curPadNew & downBit) != 0)
		{
			m->cursor++;
			moved = 1;
		}
	}

	if (m->cursor < 0)
		m->cursor = m->count - 1;
	else if (m->cursor >= m->count)
		m->cursor = 0;

	return ((selectBit != 0) && ((curPadNew & selectBit) != 0)) ? 1 : 0;
}

const char* jer_menu_label(const JerMenu* m)
{
	if (m == NULL || !m->visible)
		return "";

	/* a YESNO has no item table: 0 = "Yes", 1 = "No" */
	if (m->kind == JER_MENU_KIND_YESNO)
		return (m->cursor == 0) ? "Yes" : "No";

	if (m->items == NULL || m->cursor < 0 || m->cursor >= m->count)
		return "";

	return m->items[m->cursor];
}

const char* jer_menu_title(const JerMenu* m)
{
	if (m == NULL || m->title == NULL)
		return "";

	return m->title;
}
