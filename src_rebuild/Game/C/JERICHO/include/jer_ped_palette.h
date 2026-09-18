#ifndef JER_PED_PALETTE_H
#define JER_PED_PALETTE_H

/*
 * jer_ped_palette.h — per-instance pedestrian palettes.
 *
 * A skeleton pedestrian's colours come from its polygon CLUTs, which are shared
 * by every instance of that model. This gives ONE instance a recoloured copy of
 * those CLUTs, so a module can put individual Tanners in team colours (see
 * `combatd2`'s `team_palette`) without touching anyone else.
 *
 * How it works, and why it is per-(texture_set, texture_id): see
 * JERICHO/docs/ped-palette.md. In short - the Tanner body is one texture page
 * but two CLUT entries, so a team colour needs one recoloured row per entry, and
 * those rows are swapped into `texture_cluts` for the duration of one instance's
 * draw. Plotting only captures the CLUT address into each primitive, so nothing
 * leaks to other pedestrians.
 *
 * Lifecycle:
 *
 *     jer_ped_palette_init()           once per level, from InitTanner
 *     handle = jer_ped_palette_team(r, g, b, strength)   once per team
 *
 * and then, per ped draw, from a JER_EVENT_PED_DRAW handler:
 *
 *     jer_ped_palette_select(handle);  // or -1 for stock colours
 *
 * The host brackets the draw itself (jer_ped_palette_enter/leave), so a module
 * never has to. Calls are safe before init: with no pairs recorded they do
 * nothing.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* How many teams can be cached at once (each costs one CLUT row per body entry,
 * i.e. two rows today). */
#define JER_PED_PAL_MAX_TEAMS 16

/*
 * Walk the Tanner skeleton and remember the CLUT entries its body uses, plus
 * their stock values. Called from the engine's InitTanner, i.e. once per level
 * and before anything draws those models (the plotter rewrites poly ids in
 * place, so the walk is only faithful on a fresh model). Also drops any cached
 * teams - a level reload re-creates the VRAM rows.
 *
 * Logs the measured footprint when JER_PALETTE_PROBE is set.
 */
void jer_ped_palette_init(void);

/* Number of CLUT entries the body uses (0 before init / if the models are
 * missing). Two today: (2,13) and (2,2). */
int jer_ped_palette_pairs(void);

/*
 * Build (or reuse) the palette for a team colour and return a handle >= 0, or -1
 * if nothing was recorded or there is no VRAM left. `strength` is 0..256: 0 keeps
 * the original colours, 256 takes the team hue outright. Brightness is preserved
 * per entry, so the body keeps its shading instead of becoming a flat silhouette,
 * and black stays black. Identical (r,g,b,strength) asks reuse the same rows.
 */
int jer_ped_palette_team(int r, int g, int b, int strength);

/* Drop the cached teams (they live in rows that a level reload reuses). */
void jer_ped_palette_reset(void);

/* The palette to use for the next draw: a handle from jer_ped_palette_team, or
 * -1 for stock colours. Intended to be called from JER_EVENT_PED_DRAW. */
void jer_ped_palette_select(int handle);
int jer_ped_palette_selected(void);

/* JERICHO-HOOK: bracket one instance's draw. Called by the host in
 * newShowTanner; they must stay balanced and are no-ops when nothing is
 * selected. */
void jer_ped_palette_enter(void);
void jer_ped_palette_leave(void);

#ifdef __cplusplus
}
#endif

#endif /* JER_PED_PALETTE_H */
