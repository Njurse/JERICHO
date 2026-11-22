/* Host-side tests for jer_joinplan.c: what a refused join needs, the consent
 * question, the refusal, and the relaunch line - the engine-free half of
 * jer_joinmods. No engine, no store, no network:
 *
 *   gcc -std=c99 -Wall -Wextra -Isrc_rebuild/Game/C/JERICHO/include -o /tmp/test_joinplan \
 *       src_rebuild/Game/C/JERICHO/test/test_joinplan.c src_rebuild/Game/C/JERICHO/src/jer_joinplan.c
 *
 * (run_tests.sh builds it as C and as C++.) Exit status 0 = every check passed. */
#include <stdio.h>
#include <string.h>

#include "../src/jer_joinplan.h"

static int gFails, gChecks;

#define CHECK(cond) do { gChecks++; if (!(cond)) { gFails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static JER_JOIN_MOD Mod(const char* id, const char* version)
{
	JER_JOIN_MOD m;

	memset(&m, 0, sizeof(m));
	snprintf(m.id, sizeof(m.id), "%s", id);
	snprintf(m.version, sizeof(m.version), "%s", version);
	return m;
}

static const JER_JOINPLAN_STEP* Step(const JER_JOINPLAN* p, const char* id)
{
	int i;

	for (i = 0; i < p->count; i++)
		if (!strcmp(p->steps[i].id, id))
			return &p->steps[i];

	return NULL;
}

/* what this machine has: mp/carhacks (core, on), hallgarth (off), extra (on) */
static const JER_JOINPLAN_LOCAL kLocal[] = {
	{ "mp", "0.24.0", 1, 1, NULL },
	{ "carhacks", "0.14.0", 1, 1, NULL },
	{ "hallgarth", "1.0.0", 0, 0, NULL },
	{ "crossfire", "0.3.0", 1, 0, NULL },
	{ "broken", "1.0.0", 0, 0, "needs testmod, which is not installed" },
};
#define LOCALS ((int)(sizeof(kLocal) / sizeof(kLocal[0])))

static const char* const kRepos[] = { "JERICHO Mods", "Friends" };

static void test_switch_on_what_is_installed(void)
{
	JER_JOINPLAN p;
	JER_JOIN_MOD need[1];
	char q[200];

	need[0] = Mod("hallgarth", "1.0.0");
	jer_joinplan_build(&p, need, 1, NULL, 0, 0, kLocal, LOCALS, NULL, 0);

	CHECK(p.count == 1 && p.enables == 1 && p.blocks == 0);
	CHECK(Step(&p, "hallgarth") != NULL && Step(&p, "hallgarth")->action == JER_JOINPLAN_ENABLE);

	jer_joinplan_question(&p, kRepos, 2, q, (int)sizeof(q));
	CHECK(strstr(q, "hallgarth") != NULL);
	CHECK(strstr(q, "switched off") != NULL);
}

static void test_download_what_a_repo_offers(void)
{
	JER_JOINPLAN p;
	JER_JOIN_MOD need[1];
	JER_JOINPLAN_OFFER offers[2];
	char q[200];
	const JER_JOINPLAN_STEP* s;

	memset(offers, 0, sizeof(offers));
	offers[0].id = "bigrigs"; offers[0].name = "Big Rigs"; offers[0].version = "2.1.0";
	offers[0].repo = 1; offers[0].canInstall = 0; offers[0].note = "needs a newer JERICHO";
	offers[1].id = "bigrigs"; offers[1].name = "Big Rigs"; offers[1].version = "2.0.0";
	offers[1].repo = 0; offers[1].canInstall = 1;

	need[0] = Mod("bigrigs", "2.0.0");
	jer_joinplan_build(&p, need, 1, NULL, 0, 0, kLocal, LOCALS, offers, 2);

	s = Step(&p, "bigrigs");
	CHECK(p.downloads == 1 && p.blocks == 0);
	CHECK(s != NULL && s->action == JER_JOINPLAN_DOWNLOAD);
	CHECK(s != NULL && s->repo == 0);			/* the one the store would install */
	CHECK(s != NULL && !strcmp(s->name, "Big Rigs"));
	CHECK(s != NULL && !strcmp(s->version, "2.0.0"));

	jer_joinplan_question(&p, kRepos, 2, q, (int)sizeof(q));
	CHECK(strstr(q, "Big Rigs") != NULL);
	CHECK(strstr(q, "JERICHO Mods") != NULL);		/* the repo it comes from */
	CHECK(strstr(q, "1 mod from") != NULL);
	CHECK(strstr(q, "this session only") != NULL);
}

static void test_nothing_offers_it(void)
{
	JER_JOINPLAN p;
	JER_JOIN_MOD need[1];
	char r[200];

	need[0] = Mod("ghost", "1.0.0");
	jer_joinplan_build(&p, need, 1, NULL, 0, 0, kLocal, LOCALS, NULL, 0);

	CHECK(p.blocks == 1 && p.downloads == 0);
	jer_joinplan_refusal(&p, r, (int)sizeof(r));
	CHECK(!strncmp(r, "Can't join: ", 12));
	CHECK(strstr(r, "none of your mod repos offers ghost") != NULL);
}

static void test_exact_versions(void)
{
	JER_JOINPLAN p;
	JER_JOIN_MOD need[1];
	JER_JOINPLAN_OFFER offer;
	char r[200];

	memset(&offer, 0, sizeof(offer));
	offer.id = "hallgarth"; offer.name = "Hallgarth Cars"; offer.version = "1.1.0";
	offer.repo = 0; offer.canInstall = 1;

	/* installed at 1.0.0, server at 1.1.0, a repo offers 1.1.0: fetch it */
	need[0] = Mod("hallgarth", "1.1.0");
	jer_joinplan_build(&p, need, 1, NULL, 0, JER_JOIN_EXACT, kLocal, LOCALS, &offer, 1);
	CHECK(p.downloads == 1 && p.blocks == 0);

	/* the same without EXACT: any installed version will do, so just switch it on */
	jer_joinplan_build(&p, need, 1, NULL, 0, 0, kLocal, LOCALS, &offer, 1);
	CHECK(p.enables == 1 && p.downloads == 0);

	/* a server that names no version accepts any, even when it matches versions */
	need[0] = Mod("hallgarth", "");
	jer_joinplan_build(&p, need, 1, NULL, 0, JER_JOIN_EXACT, kLocal, LOCALS, &offer, 1);
	CHECK(p.enables == 1 && p.downloads == 0 && p.blocks == 0);
	need[0] = Mod("bigrigs", "");
	offer.id = "bigrigs";
	jer_joinplan_build(&p, need, 1, NULL, 0, JER_JOIN_EXACT, kLocal, LOCALS, &offer, 1);
	CHECK(p.downloads == 1 && p.blocks == 0);
	offer.id = "hallgarth";

	/* server at 1.2.0, nobody offers it: say so, with both versions */
	need[0] = Mod("hallgarth", "1.2.0");
	jer_joinplan_build(&p, need, 1, NULL, 0, JER_JOIN_EXACT, kLocal, LOCALS, &offer, 1);
	CHECK(p.blocks == 1);
	jer_joinplan_refusal(&p, r, (int)sizeof(r));
	CHECK(strstr(r, "1.2.0") != NULL && strstr(r, "1.0.0") != NULL);
}

static void test_switch_off_what_the_server_lacks(void)
{
	JER_JOINPLAN p;
	JER_JOIN_MOD extra[3];

	extra[0] = Mod("crossfire", "0.3.0");	/* on here, not core: off for the session */
	extra[1] = Mod("hallgarth", "1.0.0");	/* already off: nothing to do */
	extra[2] = Mod("notinstalled", "1.0");	/* not here at all: nothing to do */
	jer_joinplan_build(&p, NULL, 0, extra, 3, 0, kLocal, LOCALS, NULL, 0);

	CHECK(p.count == 1 && p.disables == 1);
	CHECK(Step(&p, "crossfire") != NULL && Step(&p, "crossfire")->action == JER_JOINPLAN_DISABLE);

	/* a core module cannot be switched off: that server cannot be joined */
	extra[0] = Mod("carhacks", "0.14.0");
	jer_joinplan_build(&p, NULL, 0, extra, 1, 0, kLocal, LOCALS, NULL, 0);
	CHECK(p.blocks == 1 && p.disables == 0);
}

static void test_what_cannot_be_fetched(void)
{
	JER_JOINPLAN p;
	JER_JOIN_MOD need[1];
	JER_JOINPLAN_OFFER offer;
	char r[200];

	memset(&offer, 0, sizeof(offer));
	offer.id = "deepmod"; offer.name = "Deep Mod"; offer.version = "1.0";
	offer.repo = 0; offer.canInstall = 1; offer.kind = JER_JOINPLAN_KIND_BUILTIN;

	need[0] = Mod("deepmod", "1.0");
	jer_joinplan_build(&p, need, 1, NULL, 0, 0, kLocal, LOCALS, &offer, 1);
	CHECK(p.blocks == 1 && p.downloads == 0);
	jer_joinplan_refusal(&p, r, (int)sizeof(r));
	CHECK(strstr(r, "built into the game") != NULL);

	/* the store would refuse it: pass its reason on */
	offer.kind = JER_JOINPLAN_KIND_UNKNOWN;
	offer.canInstall = 0;
	offer.note = "needs a newer JERICHO";
	jer_joinplan_build(&p, need, 1, NULL, 0, 0, kLocal, LOCALS, &offer, 1);
	CHECK(p.blocks == 1);
	jer_joinplan_refusal(&p, r, (int)sizeof(r));
	CHECK(strstr(r, "needs a newer JERICHO") != NULL);

	/* installed here, but this boot refused it: switching it on cannot help */
	need[0] = Mod("broken", "1.0.0");
	jer_joinplan_build(&p, need, 1, NULL, 0, 0, kLocal, LOCALS, NULL, 0);
	CHECK(p.blocks == 1 && p.enables == 0);
}

static void test_wire_rows_are_distrusted(void)
{
	JER_JOINPLAN p;
	JER_JOIN_MOD need[4];

	need[0] = Mod("hallgarth", "1.0.0");
	need[1] = Mod("hallgarth", "1.0.0");	/* a duplicate: counted once */
	need[2] = Mod("../evil", "1");		/* not an id: never planned */
	memset(&need[3], 'a', sizeof(need[3]));	/* unterminated fields */

	jer_joinplan_build(&p, need, 4, NULL, 0, 0, kLocal, LOCALS, NULL, 0);
	CHECK(Step(&p, "../evil") == NULL);
	CHECK(p.enables == 1);
	CHECK(p.count == 2);		/* hallgarth, and the 23-char "aaa..." (offered by nobody) */
	CHECK(jer_joinplan_valid_id("mp") && jer_joinplan_valid_id("_x9"));
	CHECK(!jer_joinplan_valid_id("9x") && !jer_joinplan_valid_id("a b") &&
		!jer_joinplan_valid_id("a;b") && !jer_joinplan_valid_id(""));
	CHECK(jer_joinplan_valid_address("192.168.1.20:1318") && jer_joinplan_valid_address("my-host.lan"));
	CHECK(!jer_joinplan_valid_address("1.2.3.4;rm -rf ~") && !jer_joinplan_valid_address("-level") &&
		!jer_joinplan_valid_address("a b") && !jer_joinplan_valid_address("$(x)"));
}

static void test_question_wording(void)
{
	JER_JOINPLAN p;
	JER_JOIN_MOD need[5], extra[1];
	JER_JOINPLAN_OFFER offers[5];
	static const char* ids[5] = { "m1", "m2", "m3", "m4", "m5" };
	char q[200];
	int i;

	memset(offers, 0, sizeof(offers));
	for (i = 0; i < 5; i++)
	{
		need[i] = Mod(ids[i], "1");
		offers[i].id = ids[i];
		offers[i].name = ids[i];
		offers[i].version = "1";
		offers[i].repo = i & 1;		/* two repos */
		offers[i].canInstall = 1;
	}

	jer_joinplan_build(&p, need, 5, NULL, 0, 0, kLocal, LOCALS, offers, 5);
	jer_joinplan_question(&p, kRepos, 2, q, (int)sizeof(q));
	CHECK(p.downloads == 5);
	CHECK(strstr(q, "m1, m2, m3 and 2 more") != NULL);
	CHECK(strstr(q, "your mod repos") != NULL);		/* not one repo's name */
	CHECK(strlen(q) < sizeof(q) - 1);

	need[0] = Mod("hallgarth", "1.0.0");
	extra[0] = Mod("crossfire", "0.3.0");
	jer_joinplan_build(&p, need, 1, extra, 1, 0, kLocal, LOCALS, NULL, 0);
	jer_joinplan_question(&p, kRepos, 2, q, (int)sizeof(q));
	CHECK(strstr(q, "Turn on hallgarth and off crossfire") != NULL);
}

static int Has(const JER_JOIN_MOD* rows, int n, const char* id)
{
	int i;

	for (i = 0; i < n; i++)
		if (!strcmp(rows[i].id, id))
			return 1;

	return 0;
}

static void test_diff_against_the_saved_set(void)
{
	JER_JOIN_MOD server[5], need[JER_JOIN_MAX], extra[JER_JOIN_MAX];
	static const char* const tooling[] = { "crossfire" };
	JER_JOINPLAN_LOCAL refusedOn[1];
	int nn = -1, xn = -1;

	server[0] = Mod("mp", "");
	server[1] = Mod("hallgarth", "1.0.0");
	server[2] = Mod("newmod", "2");
	server[3] = Mod("newmod", "2");		/* a duplicate row */
	server[4] = Mod("../x", "1");		/* not an id */

	/* what the server runs and this machine has off, or lacks: need */
	jer_joinplan_diff(server, 5, NULL, 0, 0, kLocal, LOCALS, need, &nn, extra, &xn);
	CHECK(nn == 2 && Has(need, nn, "hallgarth") && Has(need, nn, "newmod"));
	CHECK(xn == 0);				/* no exact set: other mods are fine */

	/* an exact set: what is on here and not listed is extra, core ones included */
	jer_joinplan_diff(server, 5, NULL, 0, JER_JOIN_EXACT_SET, kLocal, LOCALS, need, &nn, extra, &xn);
	CHECK(xn == 2 && Has(extra, xn, "carhacks") && Has(extra, xn, "crossfire"));
	CHECK(!Has(extra, xn, "hallgarth") && !Has(extra, xn, "broken"));	/* off: nothing to switch off */

	/* tooling counts neither way */
	jer_joinplan_diff(server, 5, tooling, 1, JER_JOIN_EXACT_SET, kLocal, LOCALS, need, &nn, extra, &xn);
	CHECK(xn == 1 && Has(extra, xn, "carhacks"));

	/* versions bind under EXACT only, and an empty server version means any */
	server[0] = Mod("mp", "0.25.0");
	jer_joinplan_diff(server, 1, NULL, 0, JER_JOIN_EXACT, kLocal, LOCALS, need, &nn, extra, &xn);
	CHECK(nn == 1 && Has(need, nn, "mp"));
	jer_joinplan_diff(server, 1, NULL, 0, 0, kLocal, LOCALS, need, &nn, extra, &xn);
	CHECK(nn == 0);
	server[0] = Mod("mp", "");
	jer_joinplan_diff(server, 1, NULL, 0, JER_JOIN_EXACT, kLocal, LOCALS, need, &nn, extra, &xn);
	CHECK(nn == 0);

	/* a module this boot refused is never "on": needed if listed, never extra */
	refusedOn[0].id = "shaky";
	refusedOn[0].version = "1";
	refusedOn[0].enabled = 1;
	refusedOn[0].core = 0;
	refusedOn[0].refusal = "needs a newer JERICHO";
	server[0] = Mod("shaky", "1");
	jer_joinplan_diff(server, 1, NULL, 0, 0, refusedOn, 1, need, &nn, extra, &xn);
	CHECK(nn == 1);
	jer_joinplan_diff(NULL, 0, NULL, 0, JER_JOIN_EXACT_SET, refusedOn, 1, need, &nn, extra, &xn);
	CHECK(nn == 0 && xn == 0);
}

static void test_relaunch_line(void)
{
	JER_JOINPLAN p;
	JER_JOIN_MOD need[2], extra[1];
	JER_JOINPLAN_OFFER offer;
	char line[1024];
	char* argv[] = { (char*)"jericho", (char*)"-console", (char*)"-ini", (char*)"my cfg.ini",
		(char*)"-level", (char*)"chicago", (char*)"-mod", (char*)"old", (char*)"-join",
		(char*)"9.9.9.9", (char*)"-host", (char*)"-nointro" };
	int argc = (int)(sizeof(argv) / sizeof(argv[0]));

	memset(&offer, 0, sizeof(offer));
	offer.id = "bigrigs"; offer.name = "Big Rigs"; offer.version = "2"; offer.repo = 0;
	offer.canInstall = 1;
	need[0] = Mod("hallgarth", "1.0.0");
	need[1] = Mod("bigrigs", "2");
	extra[0] = Mod("crossfire", "0.3.0");
	jer_joinplan_build(&p, need, 2, extra, 1, 0, kLocal, LOCALS, &offer, 1);

	CHECK(jer_joinplan_cmdline(&p, "/opt/j r/jericho", "10.0.0.5:1318", argc, argv, 1,
		line, (int)sizeof(line)));
	CHECK(!strcmp(line, "exec \"/opt/j r/jericho\" -console -ini \"my cfg.ini\" -nointro "
		"-join 10.0.0.5:1318 -mod hallgarth -mod bigrigs -nomod crossfire -joinmods"));

	CHECK(jer_joinplan_cmdline(&p, "C:\\Games\\JERICHO\\JERICHO.exe", "10.0.0.5", argc, argv, 0,
		line, (int)sizeof(line)));
	CHECK(!strncmp(line, "\"C:\\Games\\JERICHO\\JERICHO.exe\" -console", 39));
	CHECK(strstr(line, "-level") == NULL && strstr(line, "old") == NULL &&
		strstr(line, "9.9.9.9") == NULL && strstr(line, "-host") == NULL);

	/* what each shell would expand inside the quotes is refused outright */
	CHECK(!jer_joinplan_cmdline(&p, "/opt/$(id)/jericho", "10.0.0.5", 1, argv, 1, line, (int)sizeof(line)));
	CHECK(!jer_joinplan_cmdline(&p, "/opt/`id`/jericho", "10.0.0.5", 1, argv, 1, line, (int)sizeof(line)));
	CHECK(!jer_joinplan_cmdline(&p, "C:\\%TEMP%\\J.exe", "10.0.0.5", 1, argv, 0, line, (int)sizeof(line)));
	CHECK(!jer_joinplan_cmdline(&p, "C:\\a\"b\\J.exe", "10.0.0.5", 1, argv, 0, line, (int)sizeof(line)));
	CHECK(!jer_joinplan_cmdline(&p, "/opt/jericho", "1.2.3.4;reboot", 1, argv, 1, line, (int)sizeof(line)));

	{
		char* bad[] = { (char*)"jericho", (char*)"-cdimage", (char*)"a\"; reboot; \".iso" };

		CHECK(!jer_joinplan_cmdline(&p, "/opt/jericho", "10.0.0.5", 3, bad, 1, line, (int)sizeof(line)));
	}

	/* too small a buffer is a refusal, not a truncated command */
	CHECK(!jer_joinplan_cmdline(&p, "/opt/jericho", "10.0.0.5", 1, argv, 1, line, 40));

	/* a plan with a block never relaunches */
	need[0] = Mod("ghost", "1");
	jer_joinplan_build(&p, need, 1, NULL, 0, 0, kLocal, LOCALS, NULL, 0);
	CHECK(!jer_joinplan_cmdline(&p, "/opt/jericho", "10.0.0.5", 1, argv, 1, line, (int)sizeof(line)));
}

int main(void)
{
	test_switch_on_what_is_installed();
	test_download_what_a_repo_offers();
	test_nothing_offers_it();
	test_exact_versions();
	test_switch_off_what_the_server_lacks();
	test_what_cannot_be_fetched();
	test_wire_rows_are_distrusted();
	test_question_wording();
	test_diff_against_the_saved_set();
	test_relaunch_line();

	printf("%d checks, %d failed\n", gChecks, gFails);
	return gFails != 0;
}
