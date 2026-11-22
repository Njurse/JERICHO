/*
 * jer_joinplan.c — the engine-free half of jer_joinmods (see jer_joinplan.h).
 *
 * Every rule about WHAT a refused join needs lives here, where a plain test can
 * reach it: jer_joinmods.c only gathers the tables (the module inventory, the
 * repos' rows), asks the player, and acts on the plan this writes.
 */
#include "jer_joinplan.h"

#include <stdio.h>
#include <string.h>

static void JpCopy(char* dst, int max, const char* src)
{
	if (dst == NULL || max <= 0)
		return;

	snprintf(dst, (size_t)max, "%s", src != NULL ? src : "");
}

/* Bounded on purpose: a server's row comes off the wire in fixed fields. */
static int JpSame(const char* a, const char* b, int n)
{
	return a != NULL && b != NULL && strncmp(a, b, (size_t)n) == 0;
}

int jer_joinplan_valid_id(const char* id)
{
	const char* p;

	if (id == NULL || id[0] == 0 || strlen(id) >= JER_JOIN_ID_MAX)
		return 0;

	if (!((id[0] >= 'A' && id[0] <= 'Z') || (id[0] >= 'a' && id[0] <= 'z') || id[0] == '_'))
		return 0;

	for (p = id + 1; *p; p++)
	{
		if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
			(*p >= '0' && *p <= '9') || *p == '_'))
			return 0;
	}

	return 1;
}

int jer_joinplan_valid_address(const char* address)
{
	const char* p;

	if (address == NULL || address[0] == 0 || address[0] == '-' ||
		strlen(address) >= JER_JOIN_ADDR_MAX)
		return 0;

	for (p = address; *p; p++)
	{
		if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
			(*p >= '0' && *p <= '9') || *p == '.' || *p == ':' || *p == '-'))
			return 0;
	}

	return 1;
}

static const JER_JOINPLAN_LOCAL* JpLocal(const JER_JOINPLAN_LOCAL* local, int n, const char* id)
{
	int i;

	for (i = 0; i < n; i++)
	{
		if (JpSame(local[i].id, id, JER_JOIN_ID_MAX))
			return &local[i];
	}

	return NULL;
}

static int JpCounted(const JER_JOINPLAN* plan, const char* id)
{
	int i;

	for (i = 0; i < plan->count; i++)
	{
		if (JpSame(plan->steps[i].id, id, JER_JOIN_ID_MAX))
			return 1;
	}

	return 0;
}

static JER_JOINPLAN_STEP* JpAdd(JER_JOINPLAN* plan, int action, const char* id)
{
	JER_JOINPLAN_STEP* s;

	if (plan->count >= (int)(sizeof(plan->steps) / sizeof(plan->steps[0])))
		return NULL;

	s = &plan->steps[plan->count++];
	memset(s, 0, sizeof(*s));
	s->action = action;
	s->repo = -1;
	JpCopy(s->id, (int)sizeof(s->id), id);
	JpCopy(s->name, (int)sizeof(s->name), id);

	switch (action)
	{
	case JER_JOINPLAN_ENABLE:	plan->enables++; break;
	case JER_JOINPLAN_DISABLE:	plan->disables++; break;
	case JER_JOINPLAN_DOWNLOAD:	plan->downloads++; break;
	default:			plan->blocks++; break;
	}

	return s;
}

static void JpBlock(JER_JOINPLAN* plan, const char* id, const char* version, const char* fmt,
	const char* a, const char* b, const char* c)
{
	JER_JOINPLAN_STEP* s = JpAdd(plan, JER_JOINPLAN_BLOCK, id);

	if (s == NULL)
		return;

	JpCopy(s->version, (int)sizeof(s->version), version);
	snprintf(s->why, sizeof(s->why), fmt, a != NULL ? a : "", b != NULL ? b : "",
		c != NULL ? c : "");
}

/* A mod the server runs and this machine cannot satisfy on its own: find the
 * row a configured repo can install (at the server's version when it matches
 * versions), else say why none will do. Repos are tried in the store's order,
 * so the official repo wins a tie. */
static void JpFetch(JER_JOINPLAN* plan, const JER_JOIN_MOD* want, int exact,
	const JER_JOINPLAN_LOCAL* have, const JER_JOINPLAN_OFFER* offers, int offerCount)
{
	const JER_JOINPLAN_OFFER* named = NULL;		/* any row naming the id */
	const JER_JOINPLAN_OFFER* fits = NULL;		/* at a version the server accepts */
	const JER_JOINPLAN_OFFER* pick = NULL;		/* ...that the store would install */
	const JER_JOINPLAN_OFFER* builtin = NULL;
	int i;

	for (i = 0; i < offerCount; i++)
	{
		const JER_JOINPLAN_OFFER* o = &offers[i];

		if (!JpSame(o->id, want->id, JER_JOIN_ID_MAX))
			continue;

		if (named == NULL)
			named = o;

		/* an empty server version is "any version" (a server that names none) */
		if (exact && want->version[0] != 0 && !JpSame(o->version, want->version, JER_JOIN_VER_MAX))
			continue;

		if (fits == NULL)
			fits = o;

		if (o->kind == JER_JOINPLAN_KIND_BUILTIN)
		{
			if (builtin == NULL)
				builtin = o;
			continue;
		}

		if (o->canInstall && pick == NULL)
			pick = o;
	}

	if (pick != NULL)
	{
		JER_JOINPLAN_STEP* s = JpAdd(plan, JER_JOINPLAN_DOWNLOAD, want->id);

		if (s != NULL)
		{
			s->repo = pick->repo;
			JpCopy(s->version, (int)sizeof(s->version), pick->version);

			if (pick->name != NULL && pick->name[0] != 0)
				JpCopy(s->name, (int)sizeof(s->name), pick->name);
		}

		return;
	}

	if (named == NULL)
	{
		JpBlock(plan, want->id, want->version,
			"none of your mod repos offers %s%s%s", want->id, "", "");
		return;
	}

	if (fits == NULL)
	{
		/* exact only: the repos have the mod, not the server's version of it */
		if (have != NULL)
			JpBlock(plan, want->id, want->version,
				"the server runs %s %s; you have %s and your repos do not offer it",
				want->id, want->version, have->version);
		else
			JpBlock(plan, want->id, want->version,
				"the server runs %s %s; your repos offer %s", want->id, want->version,
				named->version);
		return;
	}

	if (builtin != NULL)
	{
		JpBlock(plan, want->id, want->version,
			"%s is built into the game; it needs a JERICHO build that has it%s%s",
			builtin->name != NULL && builtin->name[0] ? builtin->name : want->id, "", "");
		return;
	}

	if (fits->note != NULL && fits->note[0] != 0)
		JpBlock(plan, want->id, want->version, "%s: %s%s",
			fits->name != NULL && fits->name[0] ? fits->name : want->id, fits->note, "");
	else
		JpBlock(plan, want->id, want->version, "%s cannot be installed here%s%s",
			fits->name != NULL && fits->name[0] ? fits->name : want->id, "", "");
}

void jer_joinplan_build(JER_JOINPLAN* plan,
	const JER_JOIN_MOD* need, int needCount,
	const JER_JOIN_MOD* extra, int extraCount, int flags,
	const JER_JOINPLAN_LOCAL* local, int localCount,
	const JER_JOINPLAN_OFFER* offers, int offerCount)
{
	int exact = (flags & JER_JOIN_EXACT) != 0;
	int i;

	if (plan == NULL)
		return;

	memset(plan, 0, sizeof(*plan));

	if (local == NULL)
		localCount = 0;
	if (offers == NULL)
		offerCount = 0;

	for (i = 0; need != NULL && i < needCount; i++)
	{
		JER_JOIN_MOD want = need[i];
		const JER_JOINPLAN_LOCAL* have;

		/* a wire field: terminate before it is used as a string */
		want.id[sizeof(want.id) - 1] = 0;
		want.version[sizeof(want.version) - 1] = 0;

		if (!jer_joinplan_valid_id(want.id) || JpCounted(plan, want.id))
			continue;

		have = JpLocal(local, localCount, want.id);

		if (have != NULL && (!exact || want.version[0] == 0 ||
			JpSame(have->version, want.version, JER_JOIN_VER_MAX)))
		{
			if (have->refusal != NULL && have->refusal[0] != 0)
				JpBlock(plan, want.id, want.version, "%s cannot run here: %s%s",
					want.id, have->refusal, "");
			else if (!have->enabled)
				JpAdd(plan, JER_JOINPLAN_ENABLE, want.id);

			continue;
		}

		JpFetch(plan, &want, exact, have, offers, offerCount);
	}

	for (i = 0; extra != NULL && i < extraCount; i++)
	{
		JER_JOIN_MOD drop = extra[i];
		const JER_JOINPLAN_LOCAL* have;

		drop.id[sizeof(drop.id) - 1] = 0;
		drop.version[sizeof(drop.version) - 1] = 0;

		if (!jer_joinplan_valid_id(drop.id) || JpCounted(plan, drop.id))
			continue;

		have = JpLocal(local, localCount, drop.id);

		if (have == NULL || !have->enabled)
			continue;

		if (have->core)
			JpBlock(plan, drop.id, drop.version,
				"the server does not run %s, which JERICHO cannot switch off%s%s",
				drop.id, "", "");
		else
			JpAdd(plan, JER_JOINPLAN_DISABLE, drop.id);
	}
}

/* "a, b and c" from the steps of one action, at most `show` names, then
 * " and N more". */
static void JpNames(const JER_JOINPLAN* plan, int action, int show, char* out, int max)
{
	int i, listed = 0, total = 0, used = 0;

	if (max <= 0)
		return;

	out[0] = 0;

	for (i = 0; i < plan->count; i++)
		total += plan->steps[i].action == action;

	for (i = 0; i < plan->count && listed < show; i++)
	{
		const JER_JOINPLAN_STEP* s = &plan->steps[i];
		int wrote;
		const char* sep;

		if (s->action != action)
			continue;

		listed++;
		sep = listed == 1 ? "" : (listed == total ? " and " : ", ");
		wrote = snprintf(out + used, (size_t)(max - used), "%s%s", sep, s->name);

		if (wrote > 0)
			used += wrote;
		if (used >= max)
		{
			out[max - 1] = 0;
			return;
		}
	}

	if (total > listed)
		snprintf(out + used, (size_t)(max - used), " and %d more", total - listed);
}

void jer_joinplan_question(const JER_JOINPLAN* plan, const char* const* repoNames,
	int repoCount, char* out, int max)
{
	char names[96];
	char fromRepo[64];
	const char* from = "your mod repos";
	int repo = -2, i;

	if (out == NULL || max <= 0)
		return;

	out[0] = 0;

	if (plan == NULL)
		return;

	for (i = 0; i < plan->count; i++)
	{
		const JER_JOINPLAN_STEP* s = &plan->steps[i];

		if (s->action != JER_JOINPLAN_DOWNLOAD)
			continue;

		repo = (repo == -2 || repo == s->repo) ? s->repo : -1;
	}

	if (repo >= 0 && repo < repoCount && repoNames != NULL && repoNames[repo] != NULL &&
		repoNames[repo][0] != 0)
	{
		snprintf(fromRepo, sizeof(fromRepo), "the %s repo", repoNames[repo]);
		from = fromRepo;
	}

	if (plan->downloads > 0)
	{
		JpNames(plan, JER_JOINPLAN_DOWNLOAD, 3, names, (int)sizeof(names));
		snprintf(out, (size_t)max,
			"This server needs %s (%d mod%s from %s). Download and join? %s",
			names, plan->downloads, plan->downloads == 1 ? "" : "s", from,
			plan->enables + plan->disables > 0 ?
				"Your mod set changes for this session only." :
				"They are on for this session only.");
		return;
	}

	if (plan->enables > 0 && plan->disables > 0)
	{
		char off[96];

		JpNames(plan, JER_JOINPLAN_ENABLE, 2, names, (int)sizeof(names));
		JpNames(plan, JER_JOINPLAN_DISABLE, 2, off, (int)sizeof(off));
		snprintf(out, (size_t)max,
			"This server runs other mods. Turn on %s and off %s for this session and join?",
			names, off);
	}
	else if (plan->enables > 0)
	{
		JpNames(plan, JER_JOINPLAN_ENABLE, 3, names, (int)sizeof(names));
		snprintf(out, (size_t)max,
			"This server needs %s, which you have switched off. Turn on for this session and join?",
			names);
	}
	else if (plan->disables > 0)
	{
		JpNames(plan, JER_JOINPLAN_DISABLE, 3, names, (int)sizeof(names));
		snprintf(out, (size_t)max,
			"This server does not run %s. Turn off for this session and join?", names);
	}
}

void jer_joinplan_refusal(const JER_JOINPLAN* plan, char* out, int max)
{
	int i;

	if (out == NULL || max <= 0)
		return;

	out[0] = 0;

	if (plan == NULL)
		return;

	for (i = 0; i < plan->count; i++)
	{
		if (plan->steps[i].action != JER_JOINPLAN_BLOCK)
			continue;

		if (plan->blocks > 1)
			snprintf(out, (size_t)max, "Can't join: %s (and %d more)", plan->steps[i].why,
				plan->blocks - 1);
		else
			snprintf(out, (size_t)max, "Can't join: %s", plan->steps[i].why);

		return;
	}
}

/* A value written inside double quotes: what each shell would still expand
 * there is refused, so a path cannot become a command. */
static int JpQuotable(const char* s, int posix)
{
	const unsigned char* p;

	if (s == NULL || s[0] == 0)
		return 0;

	for (p = (const unsigned char*)s; *p; p++)
	{
		if (*p < 0x20 || *p == '"')
			return 0;
		if (posix && (*p == '$' || *p == '`' || *p == '\\'))
			return 0;
		if (!posix && (*p == '%' || *p == '!' || *p == '^'))
			return 0;
	}

	return 1;
}

static int JpAppend(char* out, int max, int* used, const char* text)
{
	int n = (int)strlen(text);

	if (*used + n >= max)
		return 0;

	memcpy(out + *used, text, (size_t)n + 1);
	*used += n;
	return 1;
}

int jer_joinplan_cmdline(const JER_JOINPLAN* plan, const char* exe, const char* address,
	int argc, char** argv, int posix, char* out, int max)
{
	char piece[640];
	int used = 0, i;

	if (out == NULL || max <= 0)
		return 0;

	out[0] = 0;

	if (plan == NULL || plan->blocks > 0 || !JpQuotable(exe, posix) ||
		!jer_joinplan_valid_address(address))
		return 0;

	/* sh: exec, so no shell lingers between the old game and the new one */
	snprintf(piece, sizeof(piece), "%s\"%s\"", posix ? "exec " : "", exe);
	if (!JpAppend(out, max, &used, piece))
		return 0;

	/* The original arguments worth keeping: where the game's data and config
	 * live, and how it shows itself. Everything else (a -level, a -host, the old
	 * -mod / -nomod / -join) is the plan's to decide, so it is dropped. */
	for (i = 1; argv != NULL && i < argc; i++)
	{
		const char* a = argv[i];

		if (a == NULL)
			continue;

		if (!strcmp(a, "-console") || !strcmp(a, "-nointro") || !strcmp(a, "-nofmv"))
		{
			snprintf(piece, sizeof(piece), " %s", a);
			if (!JpAppend(out, max, &used, piece))
				return 0;
		}
		else if ((!strcmp(a, "-ini") || !strcmp(a, "-cdimage")) && i + 1 < argc)
		{
			if (!JpQuotable(argv[i + 1], posix))
				return 0;	/* cannot be passed on safely, and the game needs it */

			snprintf(piece, sizeof(piece), " %s \"%s\"", a, argv[i + 1]);
			if (!JpAppend(out, max, &used, piece))
				return 0;
			i++;
		}
	}

	snprintf(piece, sizeof(piece), " -join %s", address);
	if (!JpAppend(out, max, &used, piece))
		return 0;

	for (i = 0; i < plan->count; i++)
	{
		const JER_JOINPLAN_STEP* s = &plan->steps[i];
		const char* flag;

		if (s->action == JER_JOINPLAN_ENABLE || s->action == JER_JOINPLAN_DOWNLOAD)
			flag = "-mod";
		else if (s->action == JER_JOINPLAN_DISABLE)
			flag = "-nomod";
		else
			continue;

		if (!jer_joinplan_valid_id(s->id))
			return 0;

		snprintf(piece, sizeof(piece), " %s %s", flag, s->id);
		if (!JpAppend(out, max, &used, piece))
			return 0;
	}

	return JpAppend(out, max, &used, " -joinmods");
}
