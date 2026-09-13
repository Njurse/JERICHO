#include "driver2.h"
#include "job_fx.h"
#include "bomberman.h"
#include "pause.h"
#include "gamesnd.h"
#include "cars.h"
#include "camera.h"
#include "draw.h"
#include "debris.h"
#include "system.h"

#include "jericho.h"		// JERICHO-HOOK: mod runtime (inert without modules)
#include "jer_events.h"		// JERICHO-HOOK: event argument structs

#include "ASM/rndrasm.h"

EXOBJECT explosion[MAX_EXPLOSION_OBJECTS];

MATRIX SS = { 0 };

SVECTOR globemesh[54];

void initExplosion(void);

// [D] [T]
void InitExObjects(void)
{
	int i;

	for (i = 0; i < MAX_EXPLOSION_OBJECTS; i++)
		explosion[i].time = -1;

	initExplosion();
}

// [D] [T]
void AddExplosion(VECTOR pos, int type)
{
	EXOBJECT *newExplosion;
	int i;

	i = 0;
	newExplosion = explosion;

	// Walk to the first free slot, but never off the end of the array. The
	// bound here used to be MAX_EXPLOSION_OBJECTS, so with every slot busy the
	// loop stopped one PAST the last element and the write below landed outside
	// explosion[] - quietly corrupting whatever the linker had placed after it.
	while (newExplosion->time != -1 && i < MAX_EXPLOSION_OBJECTS - 1) 
	{
		newExplosion++;
		i++;
	}

	// Still busy means the array is full. Recycle whichever slot is closest to
	// expiring instead of dropping the newest blast or trampling memory.
	if (newExplosion->time != -1)
	{
		EXOBJECT *oldest = &explosion[0];

		for (i = 1; i < MAX_EXPLOSION_OBJECTS; i++)
		{
			if (explosion[i].time > oldest->time)
				oldest = &explosion[i];
		}

		newExplosion = oldest;
	}

	newExplosion->time = 0;
	newExplosion->pos = pos;
	newExplosion->type = (ExplosionType)type;

	if (type == LITTLE_BANG) 
	{
		newExplosion->speed = 192;
		newExplosion->hscale = 1024;
		newExplosion->rscale = 1024;
	}
	else if (type == BIG_BANG)
	{
		newExplosion->speed = 128;
		newExplosion->hscale = 4096;
		newExplosion->rscale = 4096;
	}
	else if (type == HEY_MOMMA)
	{
		newExplosion->speed = 64;
		newExplosion->hscale = 16384;
		newExplosion->rscale = 16384;
	}
	else
	{
		// a module-defined type: seed the LITTLE_BANG values so the slot
		// never inherits whatever the previous explosion left in it
		newExplosion->speed = 192;
		newExplosion->hscale = 1024;
		newExplosion->rscale = 1024;
	}

	// stock FX defaults (no module attached): tint -1 keeps the stock
	// colour, collide 1 = stock push/damage, colScale 4096 = stock box
	newExplosion->fxId = 0;
	newExplosion->tintR = -1;
	newExplosion->tintG = -1;
	newExplosion->tintB = -1;
	newExplosion->fxYaw = 0;
	newExplosion->collide = 1;
	newExplosion->colScale = 4096;

	// JERICHO-HOOK: a module may attach a parametric FX profile and rewrite
	// any of these values. It also gets `type` to rewrite (usually to a stock
	// bang so the engine's sound/collision branches stay valid).
	{
		JER_ARGS_EXPLOSION_SPAWN a;

		a.pos = &newExplosion->pos;
		a.type = (int)newExplosion->type;
		a.fxId = 0;
		a.speed = newExplosion->speed;
		a.hscale = newExplosion->hscale;
		a.rscale = newExplosion->rscale;
		a.tintR = newExplosion->tintR;
		a.tintG = newExplosion->tintG;
		a.tintB = newExplosion->tintB;
		a.yawRate = 0;
		a.collide = 1;
		a.colScale = 4096;

		jer_fire(JER_EVENT_EXPLOSION_SPAWN, &a);

		newExplosion->type = (ExplosionType)a.type;
		newExplosion->fxId = a.fxId;
		newExplosion->speed = a.speed;
		newExplosion->hscale = a.hscale;
		newExplosion->rscale = a.rscale;
		newExplosion->tintR = a.tintR;
		newExplosion->tintG = a.tintG;
		newExplosion->tintB = a.tintB;
		newExplosion->fxYaw = a.yawRate;
		newExplosion->collide = a.collide;
		newExplosion->colScale = a.colScale;
	}

	// a module may have set speed to 0 (which would divide by zero in
	// ExplosionCollisionCheck/DrawExplosion and never expire) - floor it
	if (newExplosion->speed < 1)
		newExplosion->speed = 1;

}


// [D] [T]
void HandleExplosion(void)
{
	VECTOR drift;
	VECTOR smokePos;
	CAR_DATA *cp;
	EXOBJECT *exp;
	int i;

	GetSmokeDrift(&drift);	// [A]

	if (pauseflag != 0)
		return;

	exp = explosion;

	for (i = 0; i < MAX_EXPLOSION_OBJECTS; i++)
	{
		if (exp->time != -1 && exp->type != BANG_USED) 
		{
			cp = car_data;

			do {
				if (cp == &car_data[CAMERA_COLLIDER_CARID] || cp != gBombTargetVehicle && cp->controlType != CONTROL_TYPE_NONE)
					ExplosionCollisionCheck(cp, &explosion[i]);

				cp++;
			} while (cp < &car_data[MAX_CARS + 1]); // + TANNER_COLLIDER_CARID
		}

		exp++;
	}

	exp = explosion;
	for (i = 0; i < MAX_EXPLOSION_OBJECTS; i++)
	{
		if (exp->time != -1)
		{
			if (exp->time == 0)
			{
				ExplosionSound(&exp->pos, exp->type);
			}

			// [A] add smoke to explosions
			if(exp->time > 1500 && (CameraCnt & 0x3) == 0)
			{
				smokePos = exp->pos;
				smokePos.vy -= 120;
				Setup_Smoke(&smokePos, 100 + (rand() & 15) * 20, 900, SMOKE_BLACK, 0, &drift, 0);
			}

			exp->time += exp->speed;

			if (exp->time > 0xfff)
				exp->time = -1;
		}

		exp++;
	}
}

// [D] [T]
void initExplosion(void)
{
	SVECTOR *vert;
	int i;
	int d1, d2;

	// generate half-globe mesh

	vert = globemesh;
	
	d1 = 0;
	d2 = 128;

	for (i = 0; i < 18; i++)
	{
		vert[0].vy = 5;
		vert[1].vy = -265;

		vert[0].vx = FIXEDH(RCOS((d1 & 15) * 256) * 512);
		vert[0].vz = FIXEDH(RSIN((d1 & 15) * 256) * 512);

		vert[1].vx = FIXEDH(RCOS(d2) * 490);
		vert[1].vz = FIXEDH(RSIN(d2) * 490);

		vert += 2;
	
		d1 += 2;
		d2 += 512;
	}

	vert = globemesh + 18;
	
	d1 = 0x1280;
	d2 = 0x1300;

	for (i = 0; i < 18; i+=2)
	{
		vert[0].vy = -265;
		vert[1].vy = -505;

		vert[0].vx = FIXEDH(RCOS(d1) * 490);
		vert[0].vz = FIXEDH(RSIN(d1) * 490);
									 
		vert[1].vx = FIXEDH(RCOS(d2) * 330);
		vert[1].vz = FIXEDH(RSIN(d2) * 330);

		vert += 2;

		d1 += 512;
		d2 += 512;
	}

	vert = globemesh + 36;

	d1 = 0x2500;
	d2 = 9600;

	for (i = 0; i < 18; i+=2)
	{
		vert[0].vy = -505;
		vert[1].vy = -617;

		vert[0].vx = FIXEDH(RCOS(d1) * 330);
		vert[0].vz = FIXEDH(RSIN(d1) * 330);

		vert[1].vx = FIXEDH(RCOS(d2) * 100);
		vert[1].vz = FIXEDH(RSIN(d1) * 100);

		vert += 2;
		
		d1 += 512;
		d2 += 512;
	}
}


// [D] [T]
void DrawExplosion(EXOBJECT* e)
{
	int time = e->time;
	VECTOR position = e->pos;
	int hscale = e->hscale;
	int rscale = e->rscale;
	int tintR = e->tintR, tintG = e->tintG, tintB = e->tintB;
	int yaw;
	int j;
	POLY_FT4 *poly;
	SVECTOR *src;
	
	int rgb, transparency;
	int red, green, blue;
	int sf, sf1, sf2;
	
	u_int u0, u1,u2,u3;
	int i;
	VECTOR v;
	MATRIX workmatrix;
	int z;

	// JERICHO-HOOK: a module may tint/spin this explosion, or draw its own
	// effect and set override (the stock mesh is then skipped). The camera
	// matrices are live here.
	{
		JER_ARGS_EXPLOSION_DRAW a;

		// default spin: fxYaw is PSX angle units per FRAME and `time`
		// advances by e->speed per frame, so frames elapsed = time / speed
		yaw = (e->fxYaw != 0 && e->speed > 0) ? (e->fxYaw * (time / e->speed)) : 0;

		a.time = time;
		a.pos = &e->pos;
		a.hscale = hscale;
		a.rscale = rscale;
		a.tintR = tintR;
		a.tintG = tintG;
		a.tintB = tintB;
		a.yaw = yaw;
		a.fxId = e->fxId;
		a.override = 0;

		jer_fire(JER_EVENT_EXPLOSION_DRAW, &a);

		if (a.override)
			return;		// the module drew this explosion itself

		hscale = a.hscale;
		rscale = a.rscale;
		tintR = a.tintR;
		tintG = a.tintG;
		tintB = a.tintB;
		yaw = a.yaw;
	}

	u0 = *(ushort*)&smoke_texture.coords.u0 + 0x200 | *(ushort*)&smoke_texture.clutid << 0x10;
	u1 = *(ushort*)&smoke_texture.coords.u1 + 0x200 | (*(ushort*)&smoke_texture.tpageid | 0x20) << 0x10;
	u2 = *(ushort*)&smoke_texture.coords.u2 - 0x800;
	u3 = *(ushort*)&smoke_texture.coords.u3 - 0x800;

	v.vx = position.vx - camera_position.vx;
	v.vy = position.vy - camera_position.vy;
	v.vz = position.vz - camera_position.vz;

	transparency = 255 - (time >> 4);
	rgb = (transparency * transparency >> 10 << 8 |
		(255 - transparency) * (transparency >> 2) + transparency * (transparency >> 1) >> 8) << 8 | 
		transparency | 
		0x2e000000;

	if (tintR >= 0 || tintG >= 0 || tintB >= 0)
	{
		// the packed word is B<<16 | G<<8 | R (red in the low byte)
		int tb = (rgb >> 16) & 0xff, tg = (rgb >> 8) & 0xff, tr = rgb & 0xff;

		if (tintR >= 0) tr = (tr * tintR) / 255;
		if (tintG >= 0) tg = (tg * tintG) / 255;
		if (tintB >= 0) tb = (tb * tintB) / 255;

		rgb = (rgb & 0xff000000) | (tb << 16) | (tg << 8) | tr;
	}

	Apply_Inv_CameraMatrix(&v);
	gte_SetTransVector(&v);

	// [A] modify scale factor to make explosions prettier
	sf1 = FIXEDH(time * (5000 - time) * 4) + 12;
	sf2 = FIXEDH(time * (10000 - time) * 2) + 12;

	for (i = 0; i < 2; i++)
	{
		sf = CameraCnt * (64 - i * 90) + yaw;

		SS.m[1][1] = FIXED(sf1 * hscale);
		SS.m[0][0] = FIXEDH(FIXED(sf1 * rscale) * RCOS(sf));
		SS.m[2][0] = FIXEDH(FIXED(sf1 * rscale) * RSIN(sf));
		SS.m[0][2] = -SS.m[2][0];
		SS.m[2][2] = SS.m[0][0];

		MulMatrix0(&inv_camera_matrix, &SS, &workmatrix);

		gte_SetRotMatrix(&workmatrix);

		src = globemesh;

		for (j = 0; j < 12; j++)
		{
			poly = (POLY_FT4*)current->primptr;

			gte_ldv3(&src[0], &src[1], &src[2]);
			gte_rtpt();

			*(u_int*)&poly[0].r0 = rgb;
			*(u_int*)&poly[1].r0 = rgb;

			setPolyFT4(&poly[0]);
			setSemiTrans(&poly[0], 1);

			setPolyFT4(&poly[1]);
			setSemiTrans(&poly[1], 1);

			gte_stsxy3(&poly[0].x0, &poly[0].x1, &poly[0].x2);

			gte_stsxy2(&poly[1].x0);

			gte_stsz(&z);

			if (z > 32)
			{
				gte_ldv3(&src[3], &src[4], &src[5]);
				gte_rtpt();

				*(u_int*)&poly[0].u0 = u0;
				*(u_int*)&poly[0].u1 = u1;
				*(u_int*)&poly[0].u2 = u2;
				*(u_int*)&poly[0].u3 = u3;

				*(u_int*)&poly[1].u0 = u0;
				*(u_int*)&poly[1].u1 = u1;
				*(u_int*)&poly[1].u2 = u2;
				*(u_int*)&poly[1].u3 = u3;

				setPolyFT4(poly);
				setSemiTrans(poly, 1);

				setPolyFT4(&poly[1]);
				setSemiTrans(&poly[1], 1);

				gte_stsxy3(&poly[1].x1, &poly[1].x2, &poly[1].x3);

				gte_stsxy0(&poly[0].x3);

				addPrim(current->ot + (z >> 3), &poly[0]);
				addPrim(current->ot + (z >> 3), &poly[1]);

				current->primptr += sizeof(POLY_FT4) * 2;
			}

			if ((j & 3) == 3)
				src += 6;
			else
				src += 4;
		}
	}

	transparency = 255 - (time >> 4);

	rgb = transparency >> 1;
	rgb = (rgb + (transparency * transparency >> 10) >> 1 << 8 | 
			rgb + ((255 - transparency) * (transparency >> 2) + transparency * rgb >> 8) >> 1) << 8 | 
			rgb | 0x2e000000;

	if (tintR >= 0 || tintG >= 0 || tintB >= 0)
	{
		// the packed word is B<<16 | G<<8 | R (red in the low byte)
		int tb = (rgb >> 16) & 0xff, tg = (rgb >> 8) & 0xff, tr = rgb & 0xff;

		if (tintR >= 0) tr = (tr * tintR) / 255;
		if (tintG >= 0) tg = (tg * tintG) / 255;
		if (tintB >= 0) tb = (tb * tintB) / 255;

		rgb = (rgb & 0xff000000) | (tb << 16) | (tg << 8) | tr;
	}

	for (i = 0; i < 2; i++)
	{
		sf = CameraCnt * (i * -90 + 64) + yaw;
	
		SS.m[1][1] = FIXED(sf2 * hscale);
		SS.m[0][0] = FIXEDH(FIXED(sf2 * rscale) * RCOS(sf));
		SS.m[2][0] = FIXEDH(FIXED(sf2 * rscale) * RSIN(sf));
		SS.m[0][2] = -SS.m[2][0];
		SS.m[2][2] = SS.m[0][0];

		MulMatrix0(&inv_camera_matrix, &SS, &workmatrix);
		gte_SetRotMatrix(&workmatrix);

		src = globemesh;

		for (j = 0; j < 8; j++)
		{
			poly = (POLY_FT4 *)current->primptr;

			gte_ldv3(&src[0], &src[1], &src[2]);

			gte_rtpt();

			*(u_int *)&poly[1].r0 = rgb;
			*(u_int *)&poly[0].r0 = rgb;

			gte_stsxy3(&poly[0].x0, &poly[0].x1, &poly[0].x2);

			gte_stsxy2(&poly[1].x0);

			gte_stsz(&z);

			if (z > 32)
			{
				gte_ldv3(&src[3], &src[4], &src[5]);
				gte_rtpt();

				*(u_int *)&poly[0].u0 = u0;
				*(u_int *)&poly[0].u1 = u1;
				*(u_int *)&poly[0].u2 = u2;
				*(u_int *)&poly[0].u3 = u3;
				*(u_int *)&poly[1].u0 = u0;
				*(u_int *)&poly[1].u1 = u1;
				*(u_int *)&poly[1].u2 = u2;
				*(u_int *)&poly[1].u3 = u3;

				setPolyFT4(poly);
				setSemiTrans(poly, 1);

				setPolyFT4(&poly[1]);
				setSemiTrans(&poly[1], 1);

				gte_stsxy3(&poly[1].x1, &poly[1].x2, &poly[1].x3);

				gte_stsxy0(&poly[0].x3);

				addPrim(current->ot + (z >> 3), &poly[0]);
				addPrim(current->ot + (z >> 3), &poly[1]);

				current->primptr += sizeof(POLY_FT4) * 2;
			}

			if ((j & 3) == 3)
				src += 6;
			else
				src += 4;
		}
	}
}


// [D] [T]
void DrawAllExplosions(void)
{
	int i;

	for (i = 0; i < MAX_EXPLOSION_OBJECTS; i++)
	{
		if (explosion[i].time != -1)
			DrawExplosion(&explosion[i]);
	}
}




