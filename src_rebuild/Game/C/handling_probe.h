#ifndef HANDLING_PROBE_H
#define HANDLING_PROBE_H

/*
 * handling_probe.h — JERICHO: measurement for "the player's car drives way too
 * fast", so the report becomes a number instead of a feeling. See
 * handling_probe.c for what each field means and how to read it.
 *
 * Off by default: nothing here does anything unless JERICHO_HANDLING_PROBE is in
 * the environment, so a shipping build pays one cached getenv per call site.
 */

/* once per simulated step, per car (StepOneCar): samples the local player's car */
void JerHandleProbeStep(CAR_DATA* cp);

/* once per simulated step (StepGame): the sim-rate counter */
void JerHandleProbeStepGame(void);

/* end of GetFrictionScalesDriver1: the grip the tyres actually got this step */
void JerHandleProbeGrip(CAR_DATA* cp, int frontFS, int rearFS);

/* at the end of a -frames run: the machine-readable verdict line */
void JerHandleProbeSummary(void);

#endif
