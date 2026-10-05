/* GENERATED - do not edit by hand. Regenerate with
 *   python JERICHO/MODS/d1cars/tools/gen_d1citymodels.py
 *
 * Which MODELS each Driver 1 car-data city carries, as a bitmask (bit N =
 * model N), indexed by the ENGINE city index.
 *
 * WHY A TABLE AND NOT A LOAD: the frontend builds its car list BEFORE the CD
 * subsystem comes up, so nothing can be read from disk at the moment a city
 * s availability is decided - the run says "CD subsystem is not initialized
 * yet!" one step later. Driver 2 s own four cities are described by a static
 * table in FEmain.c for the same reason. This is the same thing for ours,
 * GENERATED from what the transplant actually baked rather than written out.
 */
static const unsigned short chkD1CityModels[9] = {
    0x000,  /* 0 */
    0x000,  /* 1 */
    0x000,  /* 2 */
    0x000,  /* 3 */
    0x71E,  /* 4 MIAMI */
    0x71E,  /* 5 FRISCO */
    0x71E,  /* 6 LA */
    0xF1E,  /* 7 NEWYORK */
    0x00E,  /* 8 NEWCASTLE */
};
