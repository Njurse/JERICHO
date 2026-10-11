/* jer_menu.c — the JERICHO frontend-menu library implementation.
 * See jer_menu.h. The core stays free of the game headers: the typed menus are
 * pure cursor state, and jer_menu_draw (engine-side) owns the look. */

#include "jer_menu.h"

#include <stddef.h>		/* NULL */
#include <string.h>		/* memset */

/* ---- the level stack -------------------------------------------------
 *
 * The current level lives in the menu's own fields (items/count/cursor/subs), so
 * every function that existed before this reads exactly what it always did. The
 * stack[] beside it is the SAVE area: a level is written down when the player
 * leaves it and read back when they return, cursor and all.
 */

/* Snapshot the current level into stack[depth]. Called before a descent, so the
 * level being left is remembered as it was. */
static void JerMenuSaveLevel(JerMenu* m)
{
	JerMenuLevel* lv;

	if (m->depth < 0 || m->depth >= JER_MENU_MAX_DEPTH)
		return;

	lv = &m->stack[m->depth];

	lv->items = m->items;
	lv->count = m->count;
	lv->cursor = m->cursor;
	lv->subs = m->subs;
	lv->subCount = m->subCount;
}

/* Make stack[depth] the current level. keepCursor restores where the player was
 * (coming back up); without it the level opens at its top (going down). */
static void JerMenuLoadLevel(JerMenu* m, int keepCursor)
{
	const JerMenuLevel* lv;

	if (m->depth < 0 || m->depth >= JER_MENU_MAX_DEPTH)
		return;

	lv = &m->stack[m->depth];

	m->items = lv->items;
	m->count = lv->count;
	m->subs = lv->subs;
	m->subCount = lv->subCount;

	m->cursor = keepCursor ? lv->cursor : 0;

	if (m->cursor < 0 || m->cursor >= m->count)
		m->cursor = 0;
}

/* The player chose the item under the cursor. A LEAF is reported as a select (the
 * old behaviour, bit for bit); an item WITH a submenu descends instead. */
static int JerMenuSelect(JerMenu* m)
{
	const JerMenuSub* sub;
	const char* label;

	/* Only a LIST nests. A yes/no prompt or a carousel is a single decision, and
	 * pretending a submenu belongs under one would be a menu nobody meant. */
	if (m->kind != JER_MENU_KIND_LIST || m->subs == NULL ||
	    m->cursor < 0 || m->cursor >= m->subCount)
		return JER_MENU_SELECT;

	sub = &m->subs[m->cursor];

	if (sub->items == NULL || sub->count <= 0)
		return JER_MENU_SELECT;	/* a leaf: no child declared for this item */

	if (m->depth + 1 >= JER_MENU_MAX_DEPTH)
		return JER_MENU_TOO_DEEP;	/* refused, and nothing moves */

	/* the label is kept for the breadcrumb, so a nested page can say what it is
	 * a submenu OF */
	label = (m->items != NULL && m->cursor >= 0 && m->cursor < m->count)
		? m->items[m->cursor] : NULL;

	JerMenuSaveLevel(m);

	m->depth++;
	m->stack[m->depth].items = sub->items;
	m->stack[m->depth].count = sub->count;
	m->stack[m->depth].cursor = 0;
	m->stack[m->depth].subs = sub->subs;
	m->stack[m->depth].subCount = sub->count;	/* tables are indexed by item */
	m->stack[m->depth].label = label;

	JerMenuLoadLevel(m, 0);

	return JER_MENU_OPENED;
}

void jer_menu_submenus(JerMenu* m, const JerMenuSub* subs, int count)
{
	if (m == NULL)
		return;

	/* The table is indexed by ITEM, so it is one entry per item. A table longer
	 * than the menu is a caller mistake this simply never reads past. */
	if (count > m->count)
		count = m->count;

	m->subs = subs;
	m->subCount = count;

	JerMenuSaveLevel(m);	/* the top level is a level like any other */
}

int jer_menu_back(JerMenu* m)
{
	if (m == NULL || !m->visible || m->depth <= 0)
		return 0;

	JerMenuSaveLevel(m);	/* remember this level with the cursor it had */

	m->depth--;
	JerMenuLoadLevel(m, 1);

	return 1;
}

int jer_menu_depth(const JerMenu* m)
{
	return (m == NULL) ? 0 : m->depth;
}

int jer_menu_has_sub(const JerMenu* m)
{
	if (m == NULL || !m->visible || m->kind != JER_MENU_KIND_LIST ||
	    m->subs == NULL || m->cursor < 0 || m->cursor >= m->subCount)
		return 0;

	if (m->subs[m->cursor].items == NULL || m->subs[m->cursor].count <= 0)
		return 0;

	return 1;
}

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

	/* A fresh menu is FLAT at its top level. Callers that want nesting declare it
	 * with jer_menu_submenus after this; callers that do not are untouched. */
	m->subs = NULL;
	m->subCount = 0;
	m->depth = 0;
	memset(m->stack, 0, sizeof(m->stack));

	JerMenuSaveLevel(m);
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

	return ((curPadNew & selectBit) != 0) ? JerMenuSelect(m) : JER_MENU_NOTHING;
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

	return ((selectBit != 0) && ((curPadNew & selectBit) != 0)) ? JerMenuSelect(m) : JER_MENU_NOTHING;
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
