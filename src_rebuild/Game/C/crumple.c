// MAIN CALCULATIONS FOR VERTEX DEFORMATION SYSTEM
//
// crumple.c — impact-driven car damage (body denting + wheel bending).
//
// Design (see docs/crumple.md):
//   * crumple_recordImpact() is called at collision time (car-car and
//     car-vs-world) with the world-space collision point, normal and
//     strength. It stores a per-car pending impact and accumulates wheel
//     bends for hard hits near a wheel.
//   * crumple_deform() runs inside the deferred denting pass and writes the
//     deformed vertex positions into gTempCarVertDump[cp->id], which is what
//     DrawCar uses as the car's vlist. denting.c keeps the zone-damage
//     bookkeeping and the UV damage levels; only the vertex-position block is
//     delegated here.
//   * Per-vertex structural resistance is derived once per model from the
//     clean model's AABB and cached (cabin = rigid, nose/tail/roof = soft).
//   * All math is engine 4096 fixed point (ONE / FIXEDH).
//
// The collision normal convention (verified against bcoll3d.c/handling.c):
// the engine normal points FROM the first car TOWARD the second (or from the
// car toward the wall for world hits). Deformation must always be INWARD, so
// cp0's inward direction is -normal and cp1's is +normal.

// Eventually I did get help from DeepSeek to organize this project and help with more graceful math, original prototype
// with direct hit position to vertex position to figure out deformation was written by hand :)

#include "driver2.h"
#include "crumple.h"
#include "cars.h"
#include "pad.h"
#include "players.h"

int gCrumpleBuddha = 1;
int gCrumpleGlobalScale = 4096;

//-----------------------------------------------------------------------------
// Central tuning surface — tweak everything here.
//-----------------------------------------------------------------------------
CRUMPLE_PARAMS gCrumpleParams =
{
	// impact strength
	90000,		// impactNormalize     howHard that counts as a full-scale dent
	768,		// impactMinHowHard    below this: no deformation
	42900,		// maxDisplacement     max push per impact (world units) — dramatic
					//                      crumple at the impact point (x1.65)
	11550,		// compoundCap         max TOTAL per-vertex push across all impacts (x1.65)
	0,			// damageBoostScale    zone-damage amplification (0 = impact-shaped only,
					//                      avoids zone-boundary seams)

	// falloff
	700,		// falloffRadius       dent radius (world units) — concentrated, so
					//                      a hit crumples locally instead of translating
					//                      the whole body (x2)
	1024,		// contactDeepen       extra depth for oblique (pole/edge) impacts
	1,			// falloffSmooth       0 = linear, 1 = squared

	// structure
	4096,		// cabinResistance     rigid at the cabin
	128,		// noseResistance       soft at the AABB extremes
	2048,		// resistanceInfluence  how much resistance limits movement
	2048,		// cabinSize           central ~half of the AABB stays rigid
	4096,		// globalScale         master deformation multiplier (1.0x)

	// mass / health
	384,		// massFactorMin       light-vs-heavy clamp (0.094x)
	8192,		// massFactorMax       heavy-vs-light clamp (2.0x)
	4096,		// worldImpactFactor   static/world hits
	2048,		// healthInfluence     damaged cars crumple more easily

	// surface noise
	192,		// damageModelMix      fraction of baked damaged-model delta (~4.7%)

	// wheels
	340, 		// wheelProximity      impact distance that counts as near a wheel
	30000,		// wheelMinHowHard     threshold before a wheel bends (above damage threshold ~56k... tuned down so the d-pad sim can bend wheels)
	90000,		// wheelNormalize      howHard for a full bend kick
	24,			// wheelBendStep       damage level added per full kick — 2-3 kicks
					//                      reach max, so one absolute T-bone
					//                      (a few frames of contact) breaks a wheel
	31,			// wheelBendMaxLevel   max cumulative damage level (bend = level * direction) — x0.65
	31,			// wheelBendMaxX       max lateral bend (component clamp) — x0.65
	16,			// wheelBendMaxY       max vertical bend (visual sag only) — x0.65
	16,			// wheelBendMaxZ       max toe bend (component clamp) — x0.5, so the
					//                      wheels don't distort as far longitudinally

	// impact curve
	2,			// impactCurve          squared: hard hits dent much deeper

	// wheel physics
	192,		// wheelScrubForce      ~4.7% lateral drag per unit of bend
	10,			// wheelBendCooldown    frames between wheel-bend kicks (a real
					//                      crash lasts several frames of contact)

	// wheel draw
	14506,		// wheelCamberScale     max lateral bend -> ~15deg camber (roll)
	19370,		// wheelToeScale        max longitudinal bend -> ~20deg toe (yaw)
	38827,		// wheelSteerScale      FRONT max lateral bend -> ~40deg steering deviation
	3883,		// wheelSteerScaleRear  REAR max lateral bend -> ~4deg (twist only,
					//                      must not steer the car or distort the drive)

	// debug / repair
	1000000,	// simHowHard           d-pad simulate impact strength (edge)
	90000,		// simTickHowHard       d-pad held per-frame strength (grinding)
	40,			// simJitter            simulated-hit position jitter (world units)
	2048,		// simAngleJitter       simulated-hit normal tilt (up to ~7deg)
	8,			// repairRate           zone damage removed per frame (small = slow,
					//                      watchable morph while driving)
};

//-----------------------------------------------------------------------------
// Per-car state — indexed by cp->id, same convention as gTempCarVertDump.
// No changes to CAR_DATA / APPEARANCE_DATA / any core struct.
//-----------------------------------------------------------------------------

// One recorded collision. Kept so dents COMPOUND across hits instead of
// resetting to the latest impact: nearby hits merge (same spot = stronger
// dent) and distinct hits each keep their own dent until the ring fills.
#define CRUMPLE_MAX_IMPACTS		8

typedef struct CRUMPLE_IMPACT
{
	int howHard;				// raw impact strength (accumulates on merge)
	int applied;				// deformation already baked into the persistent mesh
	int localPointX, localPointY, localPointZ;	// impact point, car-local
	int localNormalX, localNormalY, localNormalZ;	// inward normal, car-local
	int effRadius2;				// contact-aware falloff radius, squared (per pass)
	int depthScale;				// contact-aware depth boost (per pass)
} CRUMPLE_IMPACT;

typedef struct CRUMPLE_CAR_STATE
{
	int impactCount;			// active entries in impacts[] (0 = clean)
	CRUMPLE_IMPACT impacts[CRUMPLE_MAX_IMPACTS];
	VECTOR impactPoint;		// NEWEST impact, world space (overlay/debug)
	VECTOR impactNormal;	// NEWEST impact, world space, already INWARD
	int howHard;			// NEWEST impact strength (overlay/debug)
	int otherCarId;			// other car involved, -1 = world/static
	int wheelBendTimer;		// frames until the next wheel-bend kick
	// Wheel damage COMPOUNDS like body damage: a monotonic damage level per
	// wheel plus a running bend that accumulates each kick's own directional
	// push, so hits from opposite edges drive the wheel deeper while the net
	// bend partially cancels (straightens) back out.
	int wheelDamage[4];
	SVECTOR wheelDir[4];
	SVECTOR wheelBend[4];	// applied local-space offset per wheel (drawn + raycast)
} CRUMPLE_CAR_STATE;

static CRUMPLE_CAR_STATE crumpleCarState[MAX_CARS];

//-----------------------------------------------------------------------------
// Per-model cache: AABB + per-vertex structural resistance (0..4096).
//-----------------------------------------------------------------------------
typedef struct CRUMPLE_MODEL_DATA
{
	MODEL* modelPtr;		// which model the cache was built for (slot-swap guard)
	int valid;				// cache built for this model index
	int numVerts;			// cached vertex count (0 = unusable)
	int centerX, centerY, centerZ;
	short resistance[MAX_DENTING_VERTS];
} CRUMPLE_MODEL_DATA;

static CRUMPLE_MODEL_DATA crumpleModelData[MAX_CAR_RESIDENT_MODELS];

//-----------------------------------------------------------------------------
// Transform helpers
//-----------------------------------------------------------------------------

// World (engine y-down) -> car-local model space using hd.where.
// hd.where maps local->world (rotation in 4096 fixed point + int translation),
// so the inverse rotation is the transpose. levers are bounded by car size so
// the 32-bit products stay far below overflow.
static void crumpleWorldToLocal(const MATRIX* m, const VECTOR* world, int* lx, int* ly, int* lz)
{
	int dx = world->vx - m->t[0];
	int dy = world->vy - m->t[1];
	int dz = world->vz - m->t[2];

	*lx = FIXEDH(dx * m->m[0][0] + dy * m->m[1][0] + dz * m->m[2][0]);
	*ly = FIXEDH(dx * m->m[0][1] + dy * m->m[1][1] + dz * m->m[2][1]);
	*lz = FIXEDH(dx * m->m[0][2] + dy * m->m[1][2] + dz * m->m[2][2]);
}

// Direction vector (no translation) world -> car-local, transpose rotation.
static void crumpleRotateToLocal(const MATRIX* m, const VECTOR* worldDir, int* lx, int* ly, int* lz)
{
	*lx = FIXEDH(worldDir->vx * m->m[0][0] + worldDir->vy * m->m[1][0] + worldDir->vz * m->m[2][0]);
	*ly = FIXEDH(worldDir->vx * m->m[0][1] + worldDir->vy * m->m[1][1] + worldDir->vz * m->m[2][1]);
	*lz = FIXEDH(worldDir->vx * m->m[0][2] + worldDir->vy * m->m[1][2] + worldDir->vz * m->m[2][2]);
}

// Hard impact near a wheel permanently bends it: accumulate a local-space
// offset (lateral/camber + toe + lift) pushed along the inward normal.
// The wheel raycast (wheelforces.c) and the draw (cars.c) both consume it,
// so a bent wheel scrubs and fights the driver.
static void crumpleAccumulateWheelBend(CAR_DATA* cp, const VECTOR* worldPoint, int lnx, int lny, int lnz, int howHard, int wheelMask)
{
	CRUMPLE_CAR_STATE* st;
	CAR_COSMETICS* cos;
	CRUMPLE_PARAMS* p;
	int lx, ly, lz;
	int i;
	int strength;

	if (howHard < gCrumpleParams.wheelMinHowHard)
		return;

	cos = cp->ap.carCos;
	if (cos == NULL)
		return;

	st = &crumpleCarState[cp->id];
	p = &gCrumpleParams;

	// cooldown: don't re-kick on sustained contact (scraping a wall would
	// otherwise grind a wheel to maximum bend in a fraction of a second)
	if (st->wheelBendTimer > 0)
		return;

	st->wheelBendTimer = p->wheelBendCooldown;

	crumpleWorldToLocal(&cp->hd.where, worldPoint, &lx, &ly, &lz);

	strength = (int)(((long long)howHard << 12) / p->wheelNormalize);
	if (strength > 4096)
		strength = 4096;

	// wheel strength is LINEAR (unlike the body's squared impact curve):
	// a single absolute T-bone must climb toward max bend, not need dozens
	// of merged hits. A full howHard (>= wheelNormalize) -> strength 4096.

	for (i = 0; i < 4; i++)
	{
		int dx = lx - cos->wheelDisp[i].vx;
		int dy = ly - cos->wheelDisp[i].vy;
		int dz = lz - cos->wheelDisp[i].vz;
		int prox = p->wheelProximity;
		int dist2 = dx * dx + dy * dy + dz * dz;

		// wheel mask: only the listed wheels may bend (the d-pad debug sim
		// passes just its target wheels, so a front press can't skew the
		// rears through the wide proximity radius)
		if ((wheelMask & (1 << i)) == 0)
			continue;

		if (dist2 <= prox * prox)
		{
			int kick = FIXEDH(strength * p->wheelBendStep);
			SVECTOR* b = &st->wheelBend[i];

			// master multiplier (gCrumpleGlobalScale, 4096 = 1.0x)
			kick = FIXEDH(kick * gCrumpleGlobalScale);

			// distance falloff: wheels closer to the impact bend hardest, so
			// a hit spanning several wheels deforms each one appropriately
			// (full at the impact point, easing to nothing at the radius edge)
			if (prox > 0)
				kick = FIXEDH(kick * (4096 - (dist2 * 4096 / (prox * prox))));

			// compounding: the damage LEVEL always grows (deeper damage), but
			// the applied bend accumulates each kick's own DIRECTIONAL push —
			// hits from opposite edges of a wheel drive it deeper while the
			// net bend partially cancels (straightens) back out
			st->wheelDamage[i] += kick;
			if (st->wheelDamage[i] > p->wheelBendMaxLevel)
				st->wheelDamage[i] = p->wheelBendMaxLevel;

			st->wheelDir[i].vx = lnx;
			st->wheelDir[i].vy = lny;
			st->wheelDir[i].vz = lnz;

			b->vx += FIXEDH(kick * lnx);
			b->vy += FIXEDH(kick * lny);
			b->vz += FIXEDH(kick * lnz);

			if (b->vx > p->wheelBendMaxX) b->vx = p->wheelBendMaxX;
			else if (b->vx < -p->wheelBendMaxX) b->vx = -p->wheelBendMaxX;

			if (b->vy > p->wheelBendMaxY) b->vy = p->wheelBendMaxY;
			else if (b->vy < -p->wheelBendMaxY) b->vy = -p->wheelBendMaxY;

			if (b->vz > p->wheelBendMaxZ) b->vz = p->wheelBendMaxZ;
			else if (b->vz < -p->wheelBendMaxZ) b->vz = -p->wheelBendMaxZ;
		}
	}
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

void crumple_init(void)
{
	int i;

	memset(crumpleCarState, 0, sizeof(crumpleCarState));
	memset(crumpleModelData, 0, sizeof(crumpleModelData));

	for (i = 0; i < MAX_CARS; i++)
		crumpleCarState[i].otherCarId = -1;
}

void crumple_resetCar(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return;

	memset(&crumpleCarState[carId], 0, sizeof(CRUMPLE_CAR_STATE));
	crumpleCarState[carId].otherCarId = -1;
}

// Build (once per model index) the AABB and the per-vertex structural
// resistance table: t = normalized distance from the AABB centre (horizontal
// dominates, roof gets half weight); resistance = cabin - (cabin - nose) * t.
static void crumpleBuildModelData(int model)
{
	CRUMPLE_MODEL_DATA* md;
	MODEL* mdl;
	SVECTOR* v;
	int i, n;
	int minX, minY, minZ, maxX, maxY, maxZ;

	if (model < 0 || model >= MAX_CAR_RESIDENT_MODELS)
		return;

	md = &crumpleModelData[model];

	// cache is keyed by the model POINTER: car-model slots are reassigned at
	// runtime (spool.c / models.c swap gCarCleanModelPtr entries), so a valid
	// cache from a previous model in the same slot would be stale
	mdl = gCarCleanModelPtr[model];

	if (md->valid && md->modelPtr == mdl)
		return;

	md->valid = 1;
	md->modelPtr = mdl;
	md->numVerts = 0;

	if (mdl == NULL)
		return;

	n = MIN(mdl->num_vertices, MAX_DENTING_VERTS);
	if (n <= 0)
		return;

	md->numVerts = n;

	v = GET_MODEL_DATA(SVECTOR, mdl, vertices);
	if (v == NULL)
		return;

	minX = maxX = v[0].vx;
	minY = maxY = v[0].vy;
	minZ = maxZ = v[0].vz;

	for (i = 1; i < n; i++)
	{
		if (v[i].vx < minX) minX = v[i].vx;
		else if (v[i].vx > maxX) maxX = v[i].vx;

		if (v[i].vy < minY) minY = v[i].vy;
		else if (v[i].vy > maxY) maxY = v[i].vy;

		if (v[i].vz < minZ) minZ = v[i].vz;
		else if (v[i].vz > maxZ) maxZ = v[i].vz;
	}

	md->centerX = (minX + maxX) / 2;
	md->centerY = (minY + maxY) / 2;
	md->centerZ = (minZ + maxZ) / 2;

	{
		int hx = MAX(1, (maxX - minX) / 2);
		int hy = MAX(1, (maxY - minY) / 2);
		int hz = MAX(1, (maxZ - minZ) / 2);
		int cabin = gCrumpleParams.cabinResistance;
		int delta = cabin - gCrumpleParams.noseResistance;
		int cabinT = gCrumpleParams.cabinSize;

		for (i = 0; i < n; i++)
		{
			int tx = ABS(v[i].vx - md->centerX) * 4096 / hx;
			int ty = ABS(v[i].vy - md->centerY) * 4096 / hy;
			int tz = ABS(v[i].vz - md->centerZ) * 4096 / hz;
			int t;
			int u;

			if (tx > 4096) tx = 4096;
			if (ty > 4096) ty = 4096;
			if (tz > 4096) tz = 4096;

			// horizontal (nose/tail/side) dominates; roof gets half weight
			t = MAX(MAX(tx, tz), ty >> 1);

			// t <= cabinSize: inside the CABIN, fully rigid. Outside it the
			// resistance eases from cabin down to nose across the remaining
			// distance (u = 0 at the cabin edge, 1 at the AABB extremes).
			if (t <= cabinT)
			{
				md->resistance[i] = cabin;
			}
			else
			{
				u = (t - cabinT) * 4096 / MAX(1, 4096 - cabinT);
				md->resistance[i] = cabin - FIXEDH(delta * u);
			}
		}
	}
}

// Structural resistance for one vertex (0..4096, higher = more rigid).
// Falls back to the soft (nose) value for unbuildable models.
static int crumpleGetResistance(int model, int vertNo)
{
	CRUMPLE_MODEL_DATA* md;

	crumpleBuildModelData(model);

	md = &crumpleModelData[model];

	if (!md->valid || md->numVerts <= 0 || vertNo < 0 || vertNo >= md->numVerts)
		return gCrumpleParams.noseResistance;

	return md->resistance[vertNo];
}

SVECTOR* crumple_getWheelBend(int carId)
{
	if (carId < 0 || carId >= MAX_CARS)
		return NULL;

	return crumpleCarState[carId].wheelBend;
}

// Cumulative "how screwed up are the wheels" for a car, normalized to
// 0..4096 (0 = pristine, 4096 = all four wheels at their maximum bend).
// Used to amplify surface roughness etc. for damaged cars.
int crumple_getWheelDamageTotal(int carId)
{
	CRUMPLE_CAR_STATE* st;
	CRUMPLE_PARAMS* p = &gCrumpleParams;
	int maxPerWheel;
	int total = 0;
	int i;

	if (carId < 0 || carId >= MAX_CARS)
		return 0;

	st = &crumpleCarState[carId];

	for (i = 0; i < 4; i++)
	{
		total += ABS(st->wheelBend[i].vx) + ABS(st->wheelBend[i].vy) + ABS(st->wheelBend[i].vz);
	}

	maxPerWheel = p->wheelBendMaxX + p->wheelBendMaxY + p->wheelBendMaxZ;

	if (maxPerWheel > 0)
	{
		total = total * 4096 / (4 * maxPerWheel);

		if (total > 4096)
			total = 4096;
	}

	return total;
}

// Rotate a wheel's vertex buffer for camber/toe from the wheel-damage bend.
// Called from DrawCarWheels AFTER the spin rotation, with a per-wheel copy
// of the wheel model's verts (both wheels of an axle share the model buffer,
// so the damage transform needs its own copy per wheel). The wheel's local
// space: spin around X, tire in the YZ plane. Camber (lateral bend) rolls
// the tire about the forward (Z) axis; toe (longitudinal bend) yaws it about
// the vertical (Y) axis. Position/sag are handled by sWheelPos in the draw.
void crumple_transformWheelVerts(int carId, int wheelnum, SVECTOR* verts, int numVerts)
{
	SVECTOR* bend = crumple_getWheelBend(carId);
	CRUMPLE_PARAMS* p = &gCrumpleParams;
	int camberAngle, toeAngle;
	int cosC, sinC, cosT, sinT;
	int i;

	if (bend == NULL || verts == NULL)
		return;

	camberAngle = FIXEDH(bend[wheelnum].vx * p->wheelCamberScale);
	toeAngle = FIXEDH(bend[wheelnum].vz * p->wheelToeScale);

	if (camberAngle == 0 && toeAngle == 0)
		return;

	cosC = RCOS(camberAngle);
	sinC = RSIN(camberAngle);
	cosT = RCOS(toeAngle);
	sinT = RSIN(toeAngle);

	for (i = 0; i < numVerts; i++)
	{
		int x = verts[i].vx;
		int y = verts[i].vy;
		int z = verts[i].vz;

		// toe: yaw about the vertical (Y) axis
		{
			int nx = FIXEDH(x * cosT + z * sinT);
			int nz = FIXEDH(-x * sinT + z * cosT);

			x = nx;
			z = nz;
		}

		// camber: roll about the forward (Z) axis — tilts the top of the
		// tire toward/away from the car. Flip wheelCamberScale to reverse.
		{
			int nx = FIXEDH(x * cosC + y * sinC);
			int ny = FIXEDH(-x * sinC + y * cosC);

			x = nx;
			y = ny;
		}

		verts[i].vx = x;
		verts[i].vy = y;
		verts[i].vz = z;
	}
}

int crumple_hasWheelDamage(int carId)
{
	int i;
	SVECTOR* bend;

	if (carId < 0 || carId >= MAX_CARS)
		return 0;

	bend = crumpleCarState[carId].wheelBend;

	for (i = 0; i < 4; i++)
	{
		if (bend[i].vx != 0 || bend[i].vy != 0 || bend[i].vz != 0)
			return 1;
	}

	return 0;
}

void crumple_getImpactInfo(int carId, VECTOR* point, VECTOR* normal, int* howHard)
{
	if (carId < 0 || carId >= MAX_CARS)
		return;

	point->vx = crumpleCarState[carId].impactPoint.vx;
	point->vy = crumpleCarState[carId].impactPoint.vy;
	point->vz = crumpleCarState[carId].impactPoint.vz;

	normal->vx = crumpleCarState[carId].impactNormal.vx;
	normal->vy = crumpleCarState[carId].impactNormal.vy;
	normal->vz = crumpleCarState[carId].impactNormal.vz;

	*howHard = crumpleCarState[carId].howHard;
}

// Record one collision in the per-car impact ring. Dents COMPOUND across
// hits: a new impact within ~falloffRadius/2 of an existing one merges into
// it (howHard accumulates; strength saturates naturally via the curve), and
// distinct impacts each keep their own dent until the ring fills (FIFO).
static void crumpleAddImpact(CRUMPLE_CAR_STATE* st, const VECTOR* worldPoint, const VECTOR* inwardNormal, int howHard,
	int lx, int ly, int lz, int nx, int ny, int nz)
{
	CRUMPLE_PARAMS* p = &gCrumpleParams;
	int mergeR2;
	int i;

	// newest-impact mirrors (overlay / debug)
	st->impactPoint.vx = worldPoint->vx;
	st->impactPoint.vy = worldPoint->vy;
	st->impactPoint.vz = worldPoint->vz;
	st->impactNormal.vx = inwardNormal->vx;
	st->impactNormal.vy = inwardNormal->vy;
	st->impactNormal.vz = inwardNormal->vz;
	st->howHard = howHard;

	mergeR2 = (p->falloffRadius / 2) * (p->falloffRadius / 2);

	// merge with a nearby existing impact (same spot keeps getting hit)
	for (i = 0; i < st->impactCount; i++)
	{
		CRUMPLE_IMPACT* im = &st->impacts[i];
		int dx = im->localPointX - lx;
		int dy = im->localPointY - ly;
		int dz = im->localPointZ - lz;

		if (dx * dx + dy * dy + dz * dz <= mergeR2)
		{
			im->howHard += howHard;
			im->localNormalX = nx;
			im->localNormalY = ny;
			im->localNormalZ = nz;

			// the dent needs to deepen: re-apply (clamped by the impact plane,
			// so it converges instead of overshooting)
			im->applied = 0;
			return;
		}
	}

	// append, dropping the oldest entry once the ring is full
	if (st->impactCount >= CRUMPLE_MAX_IMPACTS)
	{
		memmove(&st->impacts[0], &st->impacts[1], sizeof(CRUMPLE_IMPACT) * (CRUMPLE_MAX_IMPACTS - 1));
		st->impactCount = CRUMPLE_MAX_IMPACTS - 1;
	}

	{
		CRUMPLE_IMPACT* im = &st->impacts[st->impactCount++];

		im->howHard = howHard;
		im->applied = 0;
		im->localPointX = lx;
		im->localPointY = ly;
		im->localPointZ = lz;
		im->localNormalX = nx;
		im->localNormalY = ny;
		im->localNormalZ = nz;
	}
}

void crumple_recordImpact(CAR_DATA* cp0, CAR_DATA* cp1, const VECTOR* worldPoint, const VECTOR* worldNormal, int howHard, int wheelMask)
{
	CRUMPLE_CAR_STATE* st0;
	VECTOR inward;
	int lx, ly, lz;
	int nx, ny, nz;

	if (cp0 == NULL || worldPoint == NULL || worldNormal == NULL)
		return;

	if (cp0->id < 0 || cp0->id >= MAX_CARS)
		return;

	st0 = &crumpleCarState[cp0->id];

	st0->otherCarId = (cp1 != NULL) ? CAR_INDEX(cp1) : -1;

	// Engine normal points FROM cp0 TOWARD cp1 (or from the car toward the
	// wall). Deformation must be INWARD, so cp0's inward normal = -normal.
	inward.vx = -worldNormal->vx;
	inward.vy = -worldNormal->vy;
	inward.vz = -worldNormal->vz;

	// snapshot the impact in car-local space NOW: the deferred denting pass
	// runs up to a few frames later, and the car may have rotated by then
	crumpleWorldToLocal(&cp0->hd.where, worldPoint, &lx, &ly, &lz);
	crumpleRotateToLocal(&cp0->hd.where, &inward, &nx, &ny, &nz);

	crumpleAddImpact(st0, worldPoint, &inward, howHard, lx, ly, lz, nx, ny, nz);
	crumpleAccumulateWheelBend(cp0, worldPoint, nx, ny, nz, howHard, wheelMask);

	if (cp1 != NULL)
	{
		CRUMPLE_CAR_STATE* st1;

		if (cp1->id < 0 || cp1->id >= MAX_CARS)
			return;

		st1 = &crumpleCarState[cp1->id];

		st1->otherCarId = CAR_INDEX(cp0);

		// Inward for cp1 = +normal.
		inward.vx = worldNormal->vx;
		inward.vy = worldNormal->vy;
		inward.vz = worldNormal->vz;

		crumpleWorldToLocal(&cp1->hd.where, worldPoint, &lx, &ly, &lz);
		crumpleRotateToLocal(&cp1->hd.where, &inward, &nx, &ny, &nz);

		crumpleAddImpact(st1, worldPoint, &inward, howHard, lx, ly, lz, nx, ny, nz);
		crumpleAccumulateWheelBend(cp1, worldPoint, nx, ny, nz, howHard, wheelMask);
	}
}

//-----------------------------------------------------------------------------
// Deformation
//-----------------------------------------------------------------------------

// Approximate normalize to ~4096 magnitude, no sqrt (fallback normal only).
// |v| is between m and m*sqrt(3) where m = max component, so the result is
// within ~40% of unit length worst-case — fine for a defensive fallback.
static void crumpleNormalizeApprox(int* vx, int* vy, int* vz)
{
	int m = MAX(ABS(*vx), MAX(ABS(*vy), ABS(*vz)));
	int s;

	if (m < 256)
	{
		*vx = 0;
		*vy = 0;
		*vz = 4096;
		return;
	}

	s = (1 << 24) / m;	// 12.12 fixed-point reciprocal of the max component

	*vx = FIXEDH(*vx * s);
	*vy = FIXEDH(*vy * s);
	*vz = FIXEDH(*vz * s);
}

// Degenerate impact normal (shouldn't happen in the real paths): build the
// inward direction from (nearby vertex barycenter - impact point). Returns 0
// if there are no nearby verts at all (the pass falls back to interpolation).
static int crumpleResolveImpactNormal(CRUMPLE_IMPACT* im, const SVECTOR* cleanV, int numVerts)
{
	CRUMPLE_PARAMS* p = &gCrumpleParams;
	int cx = 0, cy = 0, cz = 0, cnt = 0;
	int nLen2, r2;
	int i;

	nLen2 = im->localNormalX * im->localNormalX + im->localNormalY * im->localNormalY + im->localNormalZ * im->localNormalZ;

	if (nLen2 >= 4096)
		return 1;

	r2 = p->falloffRadius * p->falloffRadius;

	for (i = 0; i < numVerts; i++)
	{
		int dx = im->localPointX - cleanV[i].vx;
		int dy = im->localPointY - cleanV[i].vy;
		int dz = im->localPointZ - cleanV[i].vz;

		if (dx * dx + dy * dy + dz * dz <= r2)
		{
			cx += cleanV[i].vx;
			cy += cleanV[i].vy;
			cz += cleanV[i].vz;
			cnt++;
		}
	}

	if (cnt <= 0)
		return 0;

	im->localNormalX = cx / cnt - im->localPointX;
	im->localNormalY = cy / cnt - im->localPointY;
	im->localNormalZ = cz / cnt - im->localPointZ;

	crumpleNormalizeApprox(&im->localNormalX, &im->localNormalY, &im->localNormalZ);

	return 1;
}

void crumple_deform(CAR_DATA* cp, const short* tempDamage)
{
	CRUMPLE_CAR_STATE* st;
	CRUMPLE_PARAMS* p;
	MODEL* clean;
	SVECTOR* cleanV;
	SVECTOR* damV;
	SVECTOR* out;
	int numVerts;
	int model;
	int i;
	int j;

	int maxDisp;

	int healthBoost;
	int massFactor;

	// zone damage no longer shapes the procedural dent (damageBoostScale is
	// 0 — see header); the parameter is kept for the caller's convenience
	(void)tempDamage;
	if (cp == NULL)
		return;

	if (cp->id < 0 || cp->id >= MAX_CARS)
		return;

	model = cp->ap.model;

	clean = gCarCleanModelPtr[model];
	if (clean == NULL)
		return;

	numVerts = MIN(clean->num_vertices, MAX_DENTING_VERTS);
	if (numVerts <= 0)
		return;

	cleanV = GET_MODEL_DATA(SVECTOR, clean, vertices);
	if (cleanV == NULL)
		return;

	out = gTempCarVertDump[cp->id];

	damV = NULL;
	if (gCarDamModelPtr[model] != NULL)
		damV = GET_MODEL_DATA(SVECTOR, gCarDamModelPtr[model], vertices);

	st = &crumpleCarState[cp->id];
	p = &gCrumpleParams;

	// ---- no unapplied impacts: the persistent damaged mesh stays as-is ----
	if (st->impactCount <= 0)
		return;

	// ---- mass asymmetry, health (shared by all recorded impacts) ----
	massFactor = 4096;

	if (cp->ap.carCos != NULL)
	{
		int victimMass = cp->ap.carCos->mass;

		if (victimMass >= 0x7fff)
		{
			// infinite mass (cutscene etc.): does not crumple
			massFactor = 0;
		}
		else if (victimMass <= 0)
		{
			massFactor = 0;
		}
		else if (st->otherCarId >= 0 && st->otherCarId < MAX_CARS &&
				 car_data[st->otherCarId].ap.carCos != NULL &&
				 car_data[st->otherCarId].ap.carCos->mass >= 0x7fff)
		{
			// static world / infinite-mass opponent
			massFactor = p->worldImpactFactor;
		}
		else if (st->otherCarId < 0)
		{
			// world collision
			massFactor = p->worldImpactFactor;
		}
		else
		{
			int otherMass = car_data[st->otherCarId].ap.carCos->mass;

			if (otherMass <= 0)
				massFactor = p->worldImpactFactor;
			else
			{
				massFactor = (otherMass * 4096) / victimMass;

				if (massFactor < p->massFactorMin)
					massFactor = p->massFactorMin;
				else if (massFactor > p->massFactorMax)
					massFactor = p->massFactorMax;
			}
		}
	}

	// damaged cars crumple more easily
	healthBoost = FIXEDH((cp->totalDamage >> 4) * p->healthInfluence);
	if (healthBoost > 4096)
		healthBoost = 4096;

	// ---- resolve any degenerate impact normals (defense) ----
	for (j = 0; j < st->impactCount; j++)
	{
		if (!crumpleResolveImpactNormal(&st->impacts[j], cleanV, numVerts))
			return;
	}

	// ---- per-impact contact profile ----
	// Narrow objects (poles/edges) usually hit at an oblique angle, so the
	// impact normal is NOT aligned with a car face: concentrate the dent
	// (tighter radius, deeper). Wide objects (walls, car sides) hit face-on:
	// full radius, shallower. Alignment with the car-local X (side) / Z
	// (nose/tail) axes is the discriminator.
	for (j = 0; j < st->impactCount; j++)
	{
		CRUMPLE_IMPACT* im = &st->impacts[j];
		int align = MAX(ABS(im->localNormalX), ABS(im->localNormalZ));
		int widthFactor = 2048 + (align >> 1);	// 0.5x..1.0x of falloffRadius
		int effR = (p->falloffRadius * widthFactor) >> 12;

		im->effRadius2 = effR * effR;
		im->depthScale = 4096 + FIXEDH((4096 - align) * p->contactDeepen);
	}

	// ---- per-vertex deformation: bake each new impact into the persistent
	// damaged mesh ----
	// gTempCarVertDump[cp->id] IS the damaged model — it persists between
	// passes, so new impacts (any zone) ADD to the existing deformation
	// instead of recomputing it from the clean mesh (this is what kept
	// "resetting to clean" when a different panel was hit). Every impact is
	// baked ONCE (applied flag); a merge clears the flag so the dent deepens,
	// clamped by the impact plane. Repair/reset restores the clean mesh.
	maxDisp = p->maxDisplacement;

	for (j = 0; j < st->impactCount; j++)
	{
		CRUMPLE_IMPACT* im = &st->impacts[j];
		int lpx = im->localPointX;
		int lpy = im->localPointY;
		int lpz = im->localPointZ;
		int lnx = im->localNormalX;
		int lny = im->localNormalY;
		int lnz = im->localNormalZ;
		int howHard = im->howHard;
		int strength;

		if (im->applied)
			continue;

		if (howHard < p->impactMinHowHard)
			continue;

		im->applied = 1;

		strength = howHard / p->impactNormalize;
		if (strength < 0)
			strength = 0;
		else if (strength > 4096)
			strength = 4096;

		// hard/high-speed impacts deform disproportionately more
		if (p->impactCurve >= 2)
			strength = FIXEDH(strength * strength);

		for (i = 0; i < numVerts; i++)
		{
			int vx = out[i].vx;	// current (already deformed) position
			int vy = out[i].vy;
			int vz = out[i].vz;

			int ddx = lpx - vx;
			int ddy = lpy - vy;
			int ddz = lpz - vz;

			int dist2 = ddx * ddx + ddy * ddy + ddz * ddz;
			int proj, falloff;
			int resistance, softness, boost;
			int f, g, h, disp;

			if (dist2 > im->effRadius2)
				continue;

			// signed projection of the current->collision offset onto the
			// INWARD normal: positive = vertex on the attacker side of the
			// contact plane, i.e. exactly the verts that should move inward.
			// Verts exactly ON the plane (grill / tail-light silhouettes) are
			// still allowed in — they must cave with the surrounding panels;
			// only verts already past the plane are left alone.
			proj = FIXEDH(ddx * lnx + ddy * lny + ddz * lnz);

			if (proj < 0)
				continue;

			// radial falloff, optionally smoothed
			falloff = 4096 - (dist2 * 4096 / im->effRadius2);

			if (falloff < 0)
				falloff = 0;

			if (p->falloffSmooth)
				falloff = FIXEDH(falloff * falloff);

			// structural resistance (cabin rigid, nose/tail soft)
			resistance = crumpleGetResistance(model, i);
			softness = 4096 - FIXEDH(resistance * p->resistanceInfluence);
			boost = softness + healthBoost;

			if (boost > 8192)
				boost = 8192;

			f = FIXEDH(strength * falloff);
			f = FIXEDH(f * massFactor);
			g = FIXEDH(f * boost);
			h = FIXEDH(g * im->depthScale);

			// master multiplier (gCrumpleGlobalScale, 4096 = 1.0x)
			h = FIXEDH(h * gCrumpleGlobalScale);

			disp = FIXEDH(h * maxDisp);

			// clamp: hard cap, and never travel past the impact plane — EXCEPT
			// verts coplanar with the contact (the panel silhouettes): the
			// no-overshoot clamp would freeze them at zero movement, which is
			// why grill / tail-light outlines resisted denting while the rest
			// of the panels caved around them
			if (disp > maxDisp)
				disp = maxDisp;

			if (proj > 32 && disp > proj)
				disp = proj;

			if (disp > 0)
			{
				out[i].vx = vx + FIXEDH(disp * lnx);
				out[i].vy = vy + FIXEDH(disp * lny);
				out[i].vz = vz + FIXEDH(disp * lnz);

				// small fraction of the baked damage-model delta for
				// natural surface noise
				if (damV != NULL)
				{
					out[i].vx += FIXEDH((damV[i].vx - vx) * p->damageModelMix);
					out[i].vy += FIXEDH((damV[i].vy - vy) * p->damageModelMix);
					out[i].vz += FIXEDH((damV[i].vz - vz) * p->damageModelMix);
				}
			}
		}
	}

	// compounding guard: the damaged mesh may never deviate from clean by
	// more than compoundCap per axis (prevents inversion through the body)
	for (i = 0; i < numVerts; i++)
	{
		int dx = out[i].vx - cleanV[i].vx;

		if (dx > p->compoundCap)
			out[i].vx = cleanV[i].vx + p->compoundCap;
		else if (dx < -p->compoundCap)
			out[i].vx = cleanV[i].vx - p->compoundCap;

		dx = out[i].vy - cleanV[i].vy;

		if (dx > p->compoundCap)
			out[i].vy = cleanV[i].vy + p->compoundCap;
		else if (dx < -p->compoundCap)
			out[i].vy = cleanV[i].vy - p->compoundCap;

		dx = out[i].vz - cleanV[i].vz;

		if (dx > p->compoundCap)
			out[i].vz = cleanV[i].vz + p->compoundCap;
		else if (dx < -p->compoundCap)
			out[i].vz = cleanV[i].vz - p->compoundCap;
	}
}

//-----------------------------------------------------------------------------
// Pause-menu debug features: d-pad deformation simulation + smooth repair
//-----------------------------------------------------------------------------

int gCrumpleDpadDeform = 0;
int gCrumpleRepairCar = 0;

// Car-local -> world (transpose of crumpleWorldToLocal).
static void crumpleLocalToWorldPoint(const MATRIX* m, int lx, int ly, int lz, VECTOR* out)
{
	out->vx = m->t[0] + FIXEDH(lx * m->m[0][0] + ly * m->m[1][0] + lz * m->m[2][0]);
	out->vy = m->t[1] + FIXEDH(lx * m->m[0][1] + ly * m->m[1][1] + lz * m->m[2][1]);
	out->vz = m->t[2] + FIXEDH(lx * m->m[0][2] + ly * m->m[1][2] + lz * m->m[2][2]);
}

void crumple_simulateImpact(int carId, int area, int howHard)
{
	CAR_DATA* cp;
	CAR_COSMETICS* cos;
	VECTOR point, normal;
	int lpx = 0, lpy = 0, lpz = 0;
	int lnx = 0, lny = 0, lnz = 0;
	int wheelMask;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	cp = &car_data[carId];
	cos = cp->ap.carCos;

	if (cos == NULL)
		return;

	// impact points sit AT the wheels the pad points at: up/down aim at that
	// axle's two wheels, left/right at that side's two wheels, so the debug
	// pad directly bends the wheel you press toward. Normals point OUTWARD
	// from the car centre at each wheel (crumple_recordImpact flips them
	// inward for the body dent); wheel proximity + distance falloff in
	// crumpleAccumulateWheelBend then bend the near wheels hardest, so a hit
	// spanning an axle/side deforms all the wheels it touches.
	{
		int targets[2];
		int i;

		switch (area)
		{
			case 0: targets[0] = 0; targets[1] = 2; break;	// front axle
			case 1: targets[0] = 1; targets[1] = 3; break;	// rear axle
			case 2: targets[0] = 2; targets[1] = 3; break;	// left side
			case 3: targets[0] = 0; targets[1] = 1; break;	// right side
			default: return;
		}

		// bend ONLY the two wheels this press points at (mask), so a front
		// press can't skew the rear axle through the wide proximity radius
		wheelMask = (1 << targets[0]) | (1 << targets[1]);

		for (i = 0; i < 2; i++)
		{
			int w = targets[i];
			SVECTOR* wd = &cos->wheelDisp[w];

			lpx = wd->vx;
			lpy = wd->vy;
			lpz = wd->vz;

			// outward normal at this wheel: away from the car centre, horizontal
			lnx = (wd->vx > 0) ? 4096 : -4096;
			lnz = (wd->vz > 0) ? 4096 : -4096;
			lny = 0;

			// small random deviation so repeated simulations land slightly
			// off-centre and off-perpendicular (more organic than dead-on hits)
			lpx += ((rand() % (2 * gCrumpleParams.simJitter + 1)) - gCrumpleParams.simJitter);
			lpz += ((rand() % (2 * gCrumpleParams.simJitter + 1)) - gCrumpleParams.simJitter);

			lnx += ((rand() % 2049) - 1024) * gCrumpleParams.simAngleJitter >> 12;
			lny += ((rand() % 2049) - 1024) * gCrumpleParams.simAngleJitter >> 12;
			lnz += ((rand() % 2049) - 1024) * gCrumpleParams.simAngleJitter >> 12;
			crumpleNormalizeApprox(&lnx, &lny, &lnz);

			crumpleLocalToWorldPoint(&cp->hd.where, lpx, lpy, lpz, &point);

			// rotate the outward local normal into world space (no translation)
			normal.vx = FIXEDH(lnx * cp->hd.where.m[0][0] + lny * cp->hd.where.m[1][0] + lnz * cp->hd.where.m[2][0]);
			normal.vy = FIXEDH(lnx * cp->hd.where.m[0][1] + lny * cp->hd.where.m[1][1] + lnz * cp->hd.where.m[2][1]);
			normal.vz = FIXEDH(lnx * cp->hd.where.m[0][2] + lny * cp->hd.where.m[1][2] + lnz * cp->hd.where.m[2][2]);

			crumple_recordImpact(cp, NULL, &point, &normal, howHard, wheelMask);
		}
	}

	cp->ap.needsDenting = 1;
}

void crumple_debugTick(void)
{
	int i;
	int playerCarId;

	// tick the per-car wheel-bend cooldowns
	for (i = 0; i < MAX_CARS; i++)
	{
		if (crumpleCarState[i].wheelBendTimer > 0)
			crumpleCarState[i].wheelBendTimer--;
	}

	playerCarId = MainPlayer.playerCarId;

	if (playerCarId < 0 || playerCarId >= MAX_CARS)
		return;

	// ---- d-pad deformation simulation ----
	// Held directions grind the impact at a small per-frame amplitude so the
	// car crumples gradually instead of one crushing hit; the impact ring
	// merges the ticks so the dent deepens over time.
	if (gCrumpleDpadDeform)
	{
		int dpad = Pads[0].direct;
		int area = -1;

		if (dpad & MPAD_D_UP)
			area = 0;
		else if (dpad & MPAD_D_DOWN)
			area = 1;
		else if (dpad & MPAD_D_LEFT)
			area = 2;
		else if (dpad & MPAD_D_RIGHT)
			area = 3;

		if (area >= 0)
			crumple_simulateImpact(playerCarId, area, gCrumpleParams.simTickHowHard);
	}

	// ---- repair animation ----
	if (gCrumpleRepairCar)
	{
		CAR_DATA* cp = &car_data[playerCarId];
		CRUMPLE_CAR_STATE* st = &crumpleCarState[playerCarId];
		CRUMPLE_PARAMS* p = &gCrumpleParams;
		int z;
		int stillDirty = 0;
		int reduced = 0;

		// ease the six damage zones (and health) back down to 0
		for (z = 0; z < 6; z++)
		{
			int before = cp->ap.damage[z];

			if (before > 0)
			{
				int after = before - p->repairRate;

				if (after < 0)
					after = 0;

				cp->ap.damage[z] = after;
				reduced += before - after;
			}

			if (cp->ap.damage[z] > 0)
				stillDirty = 1;
		}

		if (cp->totalDamage > reduced)
			cp->totalDamage -= reduced;
		else
			cp->totalDamage = 0;

		// ease the pending impacts and wheel bends back toward zero
		for (z = 0; z < st->impactCount; z++)
		{
			if (st->impacts[z].howHard > 0)
			{
				st->impacts[z].howHard -= st->impacts[z].howHard >> 5;

				if (st->impacts[z].howHard < p->impactMinHowHard)
					st->impacts[z].howHard = 0;
			}
		}

		st->howHard -= st->howHard >> 5;

		for (z = 0; z < 4; z++)
		{
			// decay the cumulative damage level and the accumulated bend (the
			// bend IS the summed directional pushes now — nothing to
			// re-derive). A minimum decrement keeps small values from stalling
			// above zero, so repair always fully completes.
			if (st->wheelDamage[z] > 0)
				st->wheelDamage[z] -= MAX(1, st->wheelDamage[z] >> 5);

			if (st->wheelBend[z].vx > 0)
				st->wheelBend[z].vx -= MAX(1, st->wheelBend[z].vx >> 5);
			else if (st->wheelBend[z].vx < 0)
				st->wheelBend[z].vx += MAX(1, -st->wheelBend[z].vx >> 5);

			if (st->wheelBend[z].vy > 0)
				st->wheelBend[z].vy -= MAX(1, st->wheelBend[z].vy >> 5);
			else if (st->wheelBend[z].vy < 0)
				st->wheelBend[z].vy += MAX(1, -st->wheelBend[z].vy >> 5);

			if (st->wheelBend[z].vz > 0)
				st->wheelBend[z].vz -= MAX(1, st->wheelBend[z].vz >> 5);
			else if (st->wheelBend[z].vz < 0)
				st->wheelBend[z].vz += MAX(1, -st->wheelBend[z].vz >> 5);

			if (st->wheelDamage[z] <= 0)
			{
				st->wheelDamage[z] = 0;
				st->wheelBend[z].vx = 0;
				st->wheelBend[z].vy = 0;
				st->wheelBend[z].vz = 0;
				st->wheelDir[z].vx = 0;
				st->wheelDir[z].vy = 0;
				st->wheelDir[z].vz = 0;
			}
		}

		// ease the damaged mesh back toward the clean vertices (the body
		// transforms back as the panels/health do — one consistent motion)
		{
			MODEL* clean = gCarCleanModelPtr[cp->ap.model];
			SVECTOR* cleanV = NULL;
			SVECTOR* out = gTempCarVertDump[cp->id];
			int n = 0;

			if (clean != NULL)
			{
				cleanV = GET_MODEL_DATA(SVECTOR, clean, vertices);
				n = MIN(clean->num_vertices, MAX_DENTING_VERTS);
			}

			for (i = 0; i < n; i++)
			{
				out[i].vx += (cleanV[i].vx - out[i].vx) >> 3;
				out[i].vy += (cleanV[i].vy - out[i].vy) >> 3;
				out[i].vz += (cleanV[i].vz - out[i].vz) >> 3;
			}
		}

		if (stillDirty)
		{
			cp->ap.needsDenting = 1;
		}
		else
		{
			// fully repaired: restore the exact clean mesh, zero every impact
			// and bend, then run ONE final denting pass so the UV damage
			// levels settle to clean too (body, panels and lights in sync)
			MODEL* clean = gCarCleanModelPtr[cp->ap.model];
			SVECTOR* cleanV = NULL;
			SVECTOR* out = gTempCarVertDump[cp->id];
			int n = 0;

			if (clean != NULL)
			{
				cleanV = GET_MODEL_DATA(SVECTOR, clean, vertices);
				n = MIN(clean->num_vertices, MAX_DENTING_VERTS);
			}

			if (cleanV != NULL)
				memcpy(out, cleanV, n * sizeof(SVECTOR));

			st->impactCount = 0;
			st->howHard = 0;

			for (z = 0; z < 4; z++)
			{
				st->wheelBend[z].vx = 0;
				st->wheelBend[z].vy = 0;
				st->wheelBend[z].vz = 0;
				st->wheelDamage[z] = 0;
				st->wheelDir[z].vx = 0;
				st->wheelDir[z].vy = 0;
				st->wheelDir[z].vz = 0;
			}

			cp->ap.needsDenting = 1;
			gCrumpleRepairCar = 0;
		}
	}
}
