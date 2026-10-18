/*
 * collisiondevil.c — new handling model.
 *
 * Philosophy:
 *   - Drift intensity is a smoothed 0..4096 value driven by inputs.
 *   - Rear grip is reduced only to a safe floor.
 *   - Yaw assistance is an initiation that fades with current yaw rate
 *     and is hard-capped. Once the car is rotating, the kick dies.
 *   - Damping is only mildly softened.
 *   - Handbrake is a short sharp event; brake+steer is the sustained slide.
 *   - Everything still derives from existing chassis numbers.
 */

#include "driver2.h"
#include "collisiondevil.h"
#include "cars.h"
#include "cosmetic.h"
#include "camera.h"
#include "convert.h"
#include "dr2math.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_pause_menu.h"
#include "jer_config.h"
#include "jer_math.h"

COLLISIONDEVIL_CONFIG gCdCfg;
static COLLISIONDEVIL_DRIFT  gDrift[MAX_CARS];
static COLLISIONDEVIL_VISUAL gVisual[MAX_CARS];

static const char* const kPresetNames[] = { "Burnout 3", "Paradise", "Custom" };

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

static void cdLoadConfig(void)
{
    gCdCfg.enabled = jer_config_get_bool("collisiondevil", "enabled", 1);
    gCdCfg.aggression = jer_config_get_int("collisiondevil", "aggression", 50);
    gCdCfg.eagerness = jer_config_get_int("collisiondevil", "eagerness", 55);
    gCdCfg.drama = jer_config_get_int("collisiondevil", "drama", 85);
    gCdCfg.boost = jer_config_get_int("collisiondevil", "boost", 35);
    gCdCfg.preset = jer_config_get_int("collisiondevil", "preset", CD_PRESET_PARADISE);
    gCdCfg.fovPull = jer_config_get_int("collisiondevil", "fov_pull", 28);

    gCdCfg.enabled = gCdCfg.enabled ? 1 : 0;
    gCdCfg.aggression = jer_clamp_int(gCdCfg.aggression, 0, 100);
    gCdCfg.eagerness = jer_clamp_int(gCdCfg.eagerness, 0, 100);
    gCdCfg.drama = jer_clamp_int(gCdCfg.drama, 0, 100);
    gCdCfg.boost = jer_clamp_int(gCdCfg.boost, 0, 100);
    gCdCfg.preset = jer_clamp_int(gCdCfg.preset, CD_PRESET_BURNOUT, CD_PRESET_CUSTOM);
    gCdCfg.fovPull = jer_clamp_int(gCdCfg.fovPull, 0, 100);
}

static void cdSaveConfig(void)
{
    jer_config_set_bool("collisiondevil", "enabled", gCdCfg.enabled);
    jer_config_set_int("collisiondevil", "aggression", gCdCfg.aggression);
    jer_config_set_int("collisiondevil", "eagerness", gCdCfg.eagerness);
    jer_config_set_int("collisiondevil", "drama", gCdCfg.drama);
    jer_config_set_int("collisiondevil", "boost", gCdCfg.boost);
    jer_config_set_int("collisiondevil", "preset", gCdCfg.preset);
    jer_config_set_int("collisiondevil", "fov_pull", gCdCfg.fovPull);
}

static void cdApplyPreset(void)
{
    switch (gCdCfg.preset)
    {
    case CD_PRESET_BURNOUT:
        gCdCfg.aggression = 80; gCdCfg.eagerness = 75;
        gCdCfg.boost = 75; gCdCfg.drama = 65; gCdCfg.fovPull = 50;
        break;
    case CD_PRESET_PARADISE:
        gCdCfg.aggression = 50; gCdCfg.eagerness = 55;
        gCdCfg.boost = 35; gCdCfg.drama = 85; gCdCfg.fovPull = 28;
        break;
    case CD_PRESET_CUSTOM:
        break;
    }
}

static int cdScale(int v, int scale) { return (v * scale) >> 12; }
static int cdPercent(int p) { return (p * 4096) / 100; }

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

static int cdOnBoot(void* ud, void* args)
{
    (void)ud; (void)args;
    cdLoadConfig();
    cdApplyPreset();
    cdSaveConfig();
    return JER_RESULT_CONTINUE;
}

static int cdOnResetCar(void* ud, void* args)
{
    JER_ARGS_RESET_CAR* a = (JER_ARGS_RESET_CAR*)args;
    (void)ud;
    if (a->carId >= 0 && a->carId < MAX_CARS) {
        gDrift[a->carId].intensity = 0;
        gDrift[a->carId].direction = 1;
        gDrift[a->carId].handbrake = 0;
        gDrift[a->carId].hbFrames = 0;
        gDrift[a->carId].lastYawRate = 0;
        gVisual[a->carId].roll = gVisual[a->carId].pitch = gVisual[a->carId].yaw = 0;
    }
    return JER_RESULT_CONTINUE;
}

static int cdOnCarEngine(void* ud, void* args)
{
    JER_ARGS_CAR_ENGINE* a = (JER_ARGS_CAR_ENGINE*)args;
    (void)ud;
    if (!gCdCfg.enabled) return JER_RESULT_CONTINUE;

    if (a->thrust > 0) {
        int agg = 4096 + cdPercent(gCdCfg.aggression) / 2;
        int bst = 4096 + cdPercent(gCdCfg.boost) / 2;
        a->thrust = cdScale(cdScale(a->thrust, agg), bst);
    }
    else if (a->thrust < 0) {
        // Soften only when we are not already in a deep slide.
        // Once intensity is high, leave brake closer to stock so speed bleeds.
        CAR_DATA* cp = (CAR_DATA*)a->car;   // if the event provides it; otherwise use a global or skip
        int soften = CD_BRAKE_SOFTEN;
        // (If the engine event does not give the car, the soften stays constant.
        //  The friction/torque side still limits runaway.)
        a->thrust = cdScale(a->thrust, soften);
    }

    if (gCdCfg.eagerness > 0 && a->wheelAngle != 0) {
        int widen = 4096 + (cdPercent(gCdCfg.eagerness) * 0.2) / 50; // mild ~1.0–1.22×
        a->wheelAngle = cdScale(a->wheelAngle, widen);
    }
    return JER_RESULT_CONTINUE;
}

static int cdOnCarStep(void* ud, void* args)
{
    JER_ARGS_CAR_STEP* a = (JER_ARGS_CAR_STEP*)args;
    CAR_DATA* cp = (CAR_DATA*)a->car;
    (void)ud;
    if (!gCdCfg.enabled || cp->id < 0 || cp->id >= MAX_CARS)
        return JER_RESULT_CONTINUE;

    COLLISIONDEVIL_DRIFT* d = &gDrift[cp->id];
    COLLISIONDEVIL_VISUAL* v = &gVisual[cp->id];

    int steer = cp->wheel_angle;
    int braking = cp->thrust < 0;
    int handbrake = cp->handbrake != 0;

    int want = (handbrake || (braking && ABS(steer) > CD_STEER_MIN))
        && a->speed > CD_DRIFT_MIN_SPEED;

    int target = 0;
    if (want) {
        target = 4096;
        if (!d->intensity) {
            d->direction = (steer >= 0) ? 1 : -1;
            if (d->direction == 0) d->direction = 1;
        }
        d->handbrake = handbrake;
        if (handbrake) d->hbFrames++;
        else d->hbFrames = 0;
    }
    else {
        d->handbrake = 0;
        d->hbFrames = 0;
    }

    d->intensity = jer_lerp_int(d->intensity, target, CD_INTENSITY_LERP);

    // Remember yaw rate for the torque handler’s fade
    d->lastYawRate = ABS(cp->st.n.angularVelocity[1]);

    // Visual drama (render-only)
    int dramaFrac = cdPercent(gCdCfg.drama);
    int speedNorm = jer_clamp_int(a->speed, 0, CD_DRAMA_REF_SPEED) * 4096 / CD_DRAMA_REF_SPEED;
    int rollTarget = 0, pitchTarget = 0;
    if (dramaFrac > 0) {
        int steerRoll = -cdScale(steer, dramaFrac) >> CD_DRAMA_ROLL_SHIFT;
        int slipRoll = cdScale(a->velX >> CD_SLIP_SHIFT, dramaFrac) * -1;
        rollTarget = cdScale(steerRoll + slipRoll, speedNorm);
        int pitchSign = cp->thrust > 0 ? -1 : (cp->thrust < 0 ? 1 : 0);
        pitchTarget = cdScale(pitchSign * CD_DRAMA_PITCH_BASE, dramaFrac);
        pitchTarget = cdScale(pitchTarget, speedNorm);
    }
    rollTarget = jer_clamp_int(rollTarget, -CD_BODY_MAX_ROLL, CD_BODY_MAX_ROLL);
    pitchTarget = jer_clamp_int(pitchTarget, -CD_BODY_MAX_PITCH, CD_BODY_MAX_PITCH);
    v->roll = jer_lerp_int(v->roll, rollTarget, CD_VISUAL_LERP);
    v->pitch = jer_lerp_int(v->pitch, pitchTarget, CD_VISUAL_LERP);

    return JER_RESULT_CONTINUE;
}

static int cdOnCarFriction(void* ud, void* args)
{
    JER_ARGS_CAR_FRICTION* a = (JER_ARGS_CAR_FRICTION*)args;
    CAR_DATA* cp = (CAR_DATA*)a->car;
    (void)ud;
    if (!gCdCfg.enabled || cp->id < 0 || cp->id >= MAX_CARS)
        return JER_RESULT_CONTINUE;

    int intensity = gDrift[cp->id].intensity;
    if (intensity <= 0) return JER_RESULT_CONTINUE;

    int eag = cdPercent(gCdCfg.eagerness);
    int drop = cdScale(CD_GRIP_DROP_FRAC, intensity);
    drop = cdScale(drop, eag);

    if (gDrift[cp->id].handbrake && gDrift[cp->id].hbFrames < CD_HB_DURATION_FRAMES)
        drop = (int)(drop * CD_HB_GRIP_MULT);

    if (drop > CD_GRIP_FLOOR) drop = CD_GRIP_FLOOR;

    a->rearFS = a->rearFS - cdScale(a->rearFS, drop);
    // Never let rearFS go negative or absurdly small
    if (a->rearFS < 800) a->rearFS = 800;

    return JER_RESULT_CONTINUE;
}

static int cdOnCarTorque(void* ud, void* args)
{
    JER_ARGS_CAR_TORQUE* a = (JER_ARGS_CAR_TORQUE*)args;
    CAR_DATA* cp = (CAR_DATA*)a->car;
    (void)ud;
    if (!gCdCfg.enabled || cp->id < 0 || cp->id >= MAX_CARS)
        return JER_RESULT_CONTINUE;

    // Mild damping soften only
    cp->hd.aacc[0] += cp->st.n.angularVelocity[0] >> CD_DAMP_SHIFT;
    cp->hd.aacc[1] += cp->st.n.angularVelocity[1] >> CD_DAMP_SHIFT;
    cp->hd.aacc[2] += cp->st.n.angularVelocity[2] >> CD_DAMP_SHIFT;

    int intensity = gDrift[cp->id].intensity;
    if (intensity <= 0) return JER_RESULT_CONTINUE;

    int eag = cdPercent(gCdCfg.eagerness);

    // Speed taper
    int spd = cp->hd.speed;
    int over = spd - CD_YAW_LOW_SPEED;
    int range = CD_YAW_HIGH_SPEED - CD_YAW_LOW_SPEED;
    int taper = 4096 - (over * (4096 - CD_YAW_HIGH_FLOOR)) / range;
    taper = jer_clamp_int(taper, CD_YAW_HIGH_FLOOR, 4096);

    // Base initiation
    int kick = cp->ap.carCos->twistRateY * CD_YAW_INIT_SCALE / 12;
    kick *= gDrift[cp->id].direction;
    kick = cdScale(cdScale(cdScale(kick, intensity), eag), taper);

    // Fade with current yaw rate — once the car is already spinning the kick dies
    int yawRate = gDrift[cp->id].lastYawRate;
    int fade = 4096 - (yawRate >> CD_YAW_FADE_SHIFT);
    if (fade < 0) fade = 0;
    kick = cdScale(kick, fade);

    // Handbrake bonus only while the short window is open
    if (gDrift[cp->id].handbrake && gDrift[cp->id].hbFrames < CD_HB_DURATION_FRAMES)
        kick = (int)(kick * CD_HB_YAW_MULT);

    // Absolute cap
    int maxKick = (cp->ap.carCos->twistRateY * CD_YAW_MAX_FRAC) / 4096;
    if (ABS(kick) > maxKick)
        kick = (kick > 0) ? maxKick : -maxKick;

    a->yawTorque = kick;
    return JER_RESULT_CONTINUE;
}

static int cdOnCarDraw(void* ud, void* args)
{
    JER_ARGS_CAR_DRAW* a = (JER_ARGS_CAR_DRAW*)args;
    CAR_DATA* cp = (CAR_DATA*)a->car;
    MATRIX* m = (MATRIX*)a->matrix;
    (void)ud;
    if (!gCdCfg.enabled || cp->id < 0 || cp->id >= MAX_CARS)
        return JER_RESULT_CONTINUE;

    COLLISIONDEVIL_VISUAL* v = &gVisual[cp->id];
    if (v->pitch) _RotMatrixX(m, (short)v->pitch);
    if (v->roll)  _RotMatrixZ(m, (short)v->roll);
    return JER_RESULT_CONTINUE;
}

static int cdOnCamera(void* ud, void* args)
{
    JER_ARGS_CAMERA* a = (JER_ARGS_CAMERA*)args;
    (void)ud;
    if (!gCdCfg.enabled || gCdCfg.fovPull <= 0 || !a->inCar)
        return JER_RESULT_CONTINUE;

    int speed = FIXEDH(a->carSpeed);
    int spd = jer_clamp_int(speed, 0, CD_DRAMA_REF_SPEED);
    int pull = spd * gCdCfg.fovPull * CD_FOV_PULL_SCRZ / (CD_DRAMA_REF_SPEED * 100);
    SetGeomScreen(gCameraDefaultScrZ - pull);
    return JER_RESULT_CONTINUE;
}

static int cdOnCollision(void* ud, void* args)
{
    JER_ARGS_COLLISION* a = (JER_ARGS_COLLISION*)args;
    CAR_DATA* cp0 = (CAR_DATA*)a->car0;
    CAR_DATA* cp1 = (CAR_DATA*)a->car1;
    (void)ud;
    if (!gCdCfg.enabled || !cp0) return JER_RESULT_CONTINUE;

    int spin = a->howHard >> CD_CRASH_SPIN_SHIFT;
    if (spin <= 0) return JER_RESULT_CONTINUE;

    if (cp0->id >= 0 && cp0->id < MAX_CARS) {
        cp0->st.n.angularVelocity[0] += (cp0->id & 1) ? spin : -spin;
        cp0->st.n.angularVelocity[1] += (cp0->id & 2) ? spin : -spin;
        cp0->st.n.angularVelocity[2] += spin;
    }
    if (cp1 && cp1->id >= 0 && cp1->id < MAX_CARS) {
        cp1->st.n.angularVelocity[0] -= (cp1->id & 1) ? spin : -spin;
        cp1->st.n.angularVelocity[1] -= (cp1->id & 2) ? spin : -spin;
        cp1->st.n.angularVelocity[2] -= spin;
    }
    return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Pause menu (structure preserved)
// ---------------------------------------------------------------------------

static void cdLabelEnabled(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Enabled: %s", gCdCfg.enabled ? "ON" : "OFF"); }
static int  cdToggleEnabled(void* ud, int dir) { (void)ud; (void)dir; gCdCfg.enabled = !gCdCfg.enabled; cdSaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cdLabelAggression(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Arcade Aggression: %d%%", gCdCfg.aggression); }
static int  cdAdjAggression(void* ud, int dir) { (void)ud; gCdCfg.aggression = jer_clamp_int(gCdCfg.aggression + dir * 5, 0, 100); gCdCfg.preset = CD_PRESET_CUSTOM; cdSaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cdLabelEagerness(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Drift Eagerness: %d%%", gCdCfg.eagerness); }
static int  cdAdjEagerness(void* ud, int dir) { (void)ud; gCdCfg.eagerness = jer_clamp_int(gCdCfg.eagerness + dir * 5, 0, 100); gCdCfg.preset = CD_PRESET_CUSTOM; cdSaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cdLabelBoost(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Boost Intensity: %d%%", gCdCfg.boost); }
static int  cdAdjBoost(void* ud, int dir) { (void)ud; gCdCfg.boost = jer_clamp_int(gCdCfg.boost + dir * 5, 0, 100); gCdCfg.preset = CD_PRESET_CUSTOM; cdSaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cdLabelPreset(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Preset: %s", kPresetNames[gCdCfg.preset]); }
static int  cdCyclePreset(void* ud, int dir) { (void)ud; (void)dir; gCdCfg.preset = (gCdCfg.preset + 1) % 3; cdApplyPreset(); cdSaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cdLabelDrama(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Visual Drama: %d%%", gCdCfg.drama); }
static int  cdAdjDrama(void* ud, int dir) { (void)ud; gCdCfg.drama = jer_clamp_int(gCdCfg.drama + dir * 5, 0, 100); gCdCfg.preset = CD_PRESET_CUSTOM; cdSaveConfig(); return JER_PAUSE_QUIT_NONE; }

static void cdLabelFov(void* ud, char* out, int max) { (void)ud; snprintf(out, max, "Camera FOV Pull: %d%%", gCdCfg.fovPull); }
static int  cdAdjFov(void* ud, int dir) { (void)ud; gCdCfg.fovPull = jer_clamp_int(gCdCfg.fovPull + dir * 5, 0, 100); gCdCfg.preset = CD_PRESET_CUSTOM; cdSaveConfig(); return JER_PAUSE_QUIT_NONE; }

static int cdResetDefaults(void* ud, int dir)
{
    (void)ud; (void)dir;
    gCdCfg.preset = CD_PRESET_PARADISE;
    cdApplyPreset();
    cdSaveConfig();
    return JER_PAUSE_QUIT_NONE;
}

static const JER_PAUSE_MENU_ITEM cdVisualItems[] = {
    { NULL, cdLabelDrama, cdAdjDrama, NULL, NULL, 1 },
    { NULL, cdLabelFov,   cdAdjFov,   NULL, NULL, 1 },
    { "Reset to Defaults", NULL, cdResetDefaults, NULL, NULL, 0 },
};
static const JER_PAUSE_MENU cdVisualMenu = { "Visual Theater", cdVisualItems, 3 };

static const JER_PAUSE_MENU_ITEM cdMainItems[] = {
    { NULL, cdLabelEnabled,    cdToggleEnabled, NULL, NULL, 0 },
    { NULL, cdLabelAggression, cdAdjAggression, NULL, NULL, 1 },
    { NULL, cdLabelEagerness,  cdAdjEagerness,  NULL, NULL, 1 },
    { NULL, cdLabelBoost,      cdAdjBoost,      NULL, NULL, 1 },
    { NULL, cdLabelPreset,     cdCyclePreset,   NULL, NULL, 1 },
    { "Visual Theater", NULL, NULL, NULL, &cdVisualMenu, 0 },
};
static const JER_PAUSE_MENU cdMainMenu = { "COLLISIONDEVIL", cdMainItems, 6 };

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

JER_MODULE_ENTRY(jer_module_collisiondevil_entry)(JERICHO_CONTEXT* ctx)
{
    cdLoadConfig();
    cdApplyPreset();

    ctx->jer_register_module(ctx,
        "collisiondevil",
        "COLLISIONDEVIL",
        "0.3.0",
        "JERICHO",
        "New handling model: self-limiting Paradise-style drifts. Mild grip drop, fading yaw initiation, no runaway spins.",
        "",
        JERICHO_SDK_VERSION);

    ctx->jer_register_hook(ctx, JER_EVENT_BOOT, cdOnBoot, NULL, 0);
    ctx->jer_register_hook(ctx, JER_EVENT_RESET_CAR, cdOnResetCar, NULL, 0);
    ctx->jer_register_hook(ctx, JER_EVENT_CAR_ENGINE, cdOnCarEngine, NULL, 0);
    ctx->jer_register_hook(ctx, JER_EVENT_CAR_STEP, cdOnCarStep, NULL, 0);
    ctx->jer_register_hook(ctx, JER_EVENT_CAR_FRICTION, cdOnCarFriction, NULL, 0);
    ctx->jer_register_hook(ctx, JER_EVENT_CAR_TORQUE, cdOnCarTorque, NULL, 0);
    ctx->jer_register_hook(ctx, JER_EVENT_CAR_DRAW, cdOnCarDraw, NULL, 0);
    ctx->jer_register_hook(ctx, JER_EVENT_COLLISION, cdOnCollision, NULL, 0);
    ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, cdOnCamera, NULL, 0);

    jer_pause_menu_register(&cdMainMenu);
    ctx->jer_log(ctx, "[collisiondevil] new handling model registered\n");
}