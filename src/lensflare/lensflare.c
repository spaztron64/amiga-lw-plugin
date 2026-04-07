/*
 * LENSFLARE.C -- Specular Lens Flare Image Filter for LightWave 3D
 *
 * Post-render filter that detects bright specular highlights and
 * composites glow and star streaks over the rendered image.
 * Uses ImageFilterHandler for full frame buffer access.
 *
 * Uses AllocMem/FreeMem and custom helpers — no libnix runtime.
 */

#include <splug.h>
#include <lwran.h>
#include <lwpanel.h>

#include <string.h>

#include <proto/exec.h>
#include <exec/memory.h>

extern struct ExecBase *SysBase;

/* ----------------------------------------------------------------
 * Memory helpers
 * ---------------------------------------------------------------- */

static void *
plugin_alloc(unsigned long size)
{
	unsigned long *p;
	p = (unsigned long *)AllocMem(size + 4, MEMF_PUBLIC | MEMF_CLEAR);
	if (!p) return 0;
	*p = size + 4;
	return (void *)(p + 1);
}

static void
plugin_free(void *ptr)
{
	unsigned long *p;
	if (!ptr) return;
	p = ((unsigned long *)ptr) - 1;
	FreeMem(p, *p);
}

/* ----------------------------------------------------------------
 * Integer/string helpers
 * ---------------------------------------------------------------- */

static void
int_to_str(int val, char *buf, int buflen)
{
	char tmp[12];
	int  i = 0, neg = 0, len;

	if (val < 0) { neg = 1; val = -val; }
	if (val == 0) { tmp[i++] = '0'; }
	else {
		while (val > 0 && i < 11) {
			tmp[i++] = (char)('0' + (val % 10));
			val /= 10;
		}
	}
	len = neg + i;
	if (len >= buflen) len = buflen - 1;
	if (neg) buf[0] = '-';
	{
		int j;
		for (j = 0; j < i && (neg + j) < buflen - 1; j++)
			buf[neg + j] = tmp[i - 1 - j];
	}
	buf[len] = '\0';
}

static const char *
lf_parse_int(const char *s, int *val)
{
	int neg = 0;
	*val = 0;
	while (*s == ' ') s++;
	if (*s == '-') { neg = 1; s++; }
	while (*s >= '0' && *s <= '9') {
		*val = *val * 10 + (*s - '0');
		s++;
	}
	if (neg) *val = -*val;
	return s;
}

static void
lf_append_int(char *buf, int *pos, int val)
{
	char tmp[12];
	int i;
	int_to_str(val, tmp, 12);
	if (*pos > 0) buf[(*pos)++] = ' ';
	for (i = 0; tmp[i]; i++)
		buf[(*pos)++] = tmp[i];
	buf[*pos] = '\0';
}

/* ----------------------------------------------------------------
 * Pre-computed 6-point star streak directions (hexagonal)
 * ---------------------------------------------------------------- */

static const double streak_dx[6] = {
	1.000, 0.500, -0.500, -1.000, -0.500, 0.500
};
static const double streak_dy[6] = {
	0.000, 0.866, 0.866, 0.000, -0.866, -0.866
};

/* ----------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------- */

#define MAX_FLARES 8

typedef struct {
	int x, y;
	int brightness;
	double depth;
} FlareSource;

typedef struct {
	int    threshold;
	int    glowRadius;
	int    streakLength;
	int    intensity;
	int    streakCount;
} LensFlareInst;

/* ----------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------- */

static MessageFuncs *msg;

/* ----------------------------------------------------------------
 * Handler callbacks
 * ---------------------------------------------------------------- */

XCALL_(static LWInstance)
Create(LWError *err)
{
	LensFlareInst *inst;
	XCALL_INIT;

	inst = (LensFlareInst *)plugin_alloc(sizeof(LensFlareInst));
	if (!inst) return 0;

	inst->threshold   = 220;
	inst->glowRadius  = 100;
	inst->streakLength = 200;
	inst->intensity   = 80;
	inst->streakCount = 6;

	return inst;
}

XCALL_(static void)
Destroy(LensFlareInst *inst)
{
	XCALL_INIT;
	if (inst) plugin_free(inst);
}

XCALL_(static LWError)
Copy(LensFlareInst *from, LensFlareInst *to)
{
	XCALL_INIT;
	*to = *from;
	return 0;
}

XCALL_(static LWError)
Load(LensFlareInst *inst, const LWLoadState *ls)
{
	char buf[64];
	const char *p;
	int v;
	XCALL_INIT;

	buf[0] = '\0';
	(*ls->read)(ls->readData, buf, 63);
	buf[63] = '\0';
	if (buf[0] == '\0') {
		(*ls->read)(ls->readData, buf, 63);
		buf[63] = '\0';
	}
	if (!buf[0]) return 0;

	p = buf;
	p = lf_parse_int(p, &v); inst->threshold = v;
	p = lf_parse_int(p, &v); inst->glowRadius = v;
	p = lf_parse_int(p, &v); inst->streakLength = v;
	p = lf_parse_int(p, &v); inst->intensity = v;
	p = lf_parse_int(p, &v); inst->streakCount = v;

	if (inst->threshold < 0) inst->threshold = 0;
	if (inst->threshold > 255) inst->threshold = 255;

	return 0;
}

XCALL_(static LWError)
Save(LensFlareInst *inst, const LWSaveState *ss)
{
	char buf[64];
	int pos = 0;
	XCALL_INIT;

	lf_append_int(buf, &pos, inst->threshold);
	lf_append_int(buf, &pos, inst->glowRadius);
	lf_append_int(buf, &pos, inst->streakLength);
	lf_append_int(buf, &pos, inst->intensity);
	lf_append_int(buf, &pos, inst->streakCount);

	(*ss->write)(ss->writeData, buf, pos);

	return 0;
}

XCALL_(static void)
Process(LensFlareInst *inst, const FilterAccess *fa)
{
	FlareSource flares[MAX_FLARES];
	int numFlares = 0;
	int x, y, i, s;
	double inten;
	int thresh, gradR, strkLen, nStrk;

	XCALL_INIT;

	thresh  = inst->threshold;
	gradR   = inst->glowRadius;
	strkLen = inst->streakLength;
	inten   = inst->intensity / 100.0;
	nStrk   = inst->streakCount;
	if (nStrk > 6) nStrk = 6;

	for (y = 0; y < fa->height; y++) {
		BufferValue *rLine = (*fa->bufLine)(LWBUF_RED, y);
		BufferValue *gLine = (*fa->bufLine)(LWBUF_GREEN, y);
		BufferValue *bLine = (*fa->bufLine)(LWBUF_BLUE, y);
		BufferValue *dLine = (*fa->bufLine)(LWBUF_DEPTH, y);
		if (!rLine || !gLine || !bLine) continue;

		for (x = 0; x < fa->width; x++) {
			int bright = ((int)rLine[x] + (int)gLine[x] + (int)bLine[x]) / 3;
			if (bright >= thresh) {
				double d = (dLine && dLine[x] > 0.0f)
				         ? (double)dLine[x] : 1.0;
				if (numFlares < MAX_FLARES) {
					flares[numFlares].x = x;
					flares[numFlares].y = y;
					flares[numFlares].brightness = bright;
					flares[numFlares].depth = d;
					numFlares++;
				} else {
					int weakest = 0, wi;
					for (wi = 1; wi < MAX_FLARES; wi++) {
						if (flares[wi].brightness < flares[weakest].brightness)
							weakest = wi;
					}
					if (bright > flares[weakest].brightness) {
						flares[weakest].x = x;
						flares[weakest].y = y;
						flares[weakest].brightness = bright;
						flares[weakest].depth = d;
					}
				}
			}
		}
	}

	for (i = 0; i < numFlares; i++) {
		double dscale = 1.0 / (1.0 + flares[i].depth * 0.1);
		flares[i].x |= 0;
		{
			int sg = (int)(gradR * dscale);
			int sl = (int)(strkLen * dscale);
			if (sg < 4) sg = 4;
			if (sl < 4) sl = 4;
			flares[i].brightness |= (sg << 16);
			flares[i].depth = (double)(sg * sg);
			flares[i].y = flares[i].y | (sl << 16);
		}
	}

	for (y = 0; y < fa->height; y++) {
		BufferValue *rLine = (*fa->bufLine)(LWBUF_RED, y);
		BufferValue *gLine = (*fa->bufLine)(LWBUF_GREEN, y);
		BufferValue *bLine = (*fa->bufLine)(LWBUF_BLUE, y);
		if (!rLine || !gLine || !bLine) continue;

		for (x = 0; x < fa->width; x++) {
			BufferValue rgb[3];
			double fR = 0.0, fG = 0.0, fB = 0.0;

			for (i = 0; i < numFlares; i++) {
				int fx = flares[i].x & 0xFFFF;
				int fy = flares[i].y & 0xFFFF;
				int sGlow = (flares[i].brightness >> 16) & 0x7FFF;
				int sStrk = (flares[i].y >> 16) & 0x7FFF;
				double sGlow2 = flares[i].depth;
				double bright = (double)(flares[i].brightness & 0xFF) / 255.0;
				double dx = (double)(x - fx);
				double dy = (double)(y - fy);
				double r2 = dx * dx + dy * dy;
				double maxR = (double)(sGlow * 4);
				double tR = 0.0, tG = 0.0, tB = 0.0;

				if (sStrk > sGlow * 4) maxR = (double)sStrk;
				if (r2 > maxR * maxR) continue;

				if (r2 < 9.0) {
					double core = inten * bright;
					tR += core; tG += core; tB += core;
				}

				{
					double gd = 1.0 + r2 / sGlow2;
					double glow = inten * bright * 0.8 / (gd * gd);
					tR += glow;
					tG += glow * 0.8;
					tB += glow * 0.6;
				}

				{
					double ringR2 = sGlow2 * 0.5;
					double rdiff = r2 - ringR2;
					if (rdiff < 0.0) rdiff = -rdiff;
					if (rdiff < ringR2 * 0.3) {
						double ringI = (1.0 - rdiff / (ringR2 * 0.3))
						             * inten * bright * 0.5;
						tR += ringI * 1.0;
						tG += ringI * 0.3;
						tB += ringI * 0.8;
					}
				}

				{
					double ax = dx > 0 ? dx : -dx;
					if (dy > -4.0 && dy < 4.0 && ax < (double)sStrk) {
						double hfade = 1.0 - ax / (double)sStrk;
						double hw = 1.0 / (1.0 + dy * dy * 0.3);
						double h = hfade * hw * inten * bright * 0.5;
						tR += h * 0.6;
						tG += h * 0.8;
						tB += h * 1.0;
					}
				}

				for (s = 0; s < nStrk; s++) {
					double along = dx * streak_dx[s] + dy * streak_dy[s];
					double perp  = dx * streak_dy[s] - dy * streak_dx[s];
					double p2 = perp * perp;

					if (along > 0.0 && along < (double)sStrk && p2 < 16.0) {
						double fade = 1.0 - along / (double)sStrk;
						double width = 1.0 / (1.0 + p2 * 0.5);
						double sk = fade * width * inten * bright * 0.6;
						tR += sk; tG += sk * 0.85; tB += sk * 0.7;
					}
				}

				if (tR > fR) fR = tR;
				if (tG > fG) fG = tG;
				if (tB > fB) fB = tB;
			}

			if (fR > 0.001 || fG > 0.001 || fB > 0.001) {
				int rv = rLine[x] + (int)(fR * 255.0);
				int gv = gLine[x] + (int)(fG * 255.0);
				int bv = bLine[x] + (int)(fB * 255.0);
				if (rv > 255) rv = 255;
				if (gv > 255) gv = 255;
				if (bv > 255) bv = 255;
				rgb[0] = (BufferValue)rv;
				rgb[1] = (BufferValue)gv;
				rgb[2] = (BufferValue)bv;
			} else {
				rgb[0] = rLine[x];
				rgb[1] = gLine[x];
				rgb[2] = bLine[x];
			}
			(*fa->setRGB)(x, y, rgb);
		}
	}
}

XCALL_(static unsigned int)
Flags(LensFlareInst *inst)
{
	XCALL_INIT;
	return 0;
}

/* ----------------------------------------------------------------
 * Interface
 * ---------------------------------------------------------------- */

static const char *streakItems[] = { "2", "4", "6", 0 };
static int streakValues[] = { 2, 4, 6 };

XCALL_(static int)
Interface(
	long             version,
	GlobalFunc      *global,
	LensFlareInst   *inst,
	void            *serverData)
{
	LWPanelFuncs *panl;
	LWPanelID     pan;
	LWControl    *ctlThresh, *ctlGlow, *ctlStreak, *ctlInten, *ctlStrkN;
	int           strkIdx;

	XCALL_INIT;
	if (version != 1)
		return AFUNC_BADVERSION;

	msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	panl = (LWPanelFuncs *)(*global)(PANEL_SERVICES_NAME, GFUSE_TRANSIENT);

	if (panl) {
		static LWPanControlDesc desc;
		static LWValue ival = {LWT_INTEGER};
		static LWValue fval = {LWT_FLOAT};
		(void)fval;

		pan = PAN_CREATE(panl, "LensFlare v" PLUGIN_VERSION
		                       " (c) D. Panokostas");
		if (!pan) return AFUNC_OK;

		ctlThresh = SLIDER_CTL(panl, pan, "Threshold", 150, 0, 255);
		ctlGlow   = INT_CTL(panl, pan, "Glow Radius");
		ctlStreak = INT_CTL(panl, pan, "Streak Length");
		ctlInten  = SLIDER_CTL(panl, pan, "Intensity", 150, 0, 100);
		ctlStrkN  = POPUP_CTL(panl, pan, "Streaks", streakItems);

		SET_INT(ctlThresh, inst->threshold);
		SET_INT(ctlGlow, inst->glowRadius);
		SET_INT(ctlStreak, inst->streakLength);
		SET_INT(ctlInten, inst->intensity);
		strkIdx = (inst->streakCount <= 2) ? 0
		        : (inst->streakCount <= 4) ? 1 : 2;
		SET_INT(ctlStrkN, strkIdx);

		if ((*panl->open)(pan, PANF_BLOCKING | PANF_CANCEL)) {
			GET_INT(ctlThresh, inst->threshold);
			GET_INT(ctlGlow, inst->glowRadius);
			GET_INT(ctlStreak, inst->streakLength);
			GET_INT(ctlInten, inst->intensity);
			GET_INT(ctlStrkN, strkIdx);
			inst->streakCount = (strkIdx < 3)
			                  ? streakValues[strkIdx] : 6;

			if (inst->threshold < 0) inst->threshold = 0;
			if (inst->threshold > 255) inst->threshold = 255;
			if (inst->glowRadius < 1) inst->glowRadius = 1;
			if (inst->glowRadius > 200) inst->glowRadius = 200;
			if (inst->streakLength < 1) inst->streakLength = 1;
			if (inst->streakLength > 400) inst->streakLength = 400;
			if (inst->intensity < 0) inst->intensity = 0;
			if (inst->intensity > 100) inst->intensity = 100;
		}

		PAN_KILL(panl, pan);
	}

	return AFUNC_OK;
}

/* ----------------------------------------------------------------
 * Activation
 * ---------------------------------------------------------------- */

XCALL_(int)
Activate(
	long         version,
	GlobalFunc  *global,
	void        *local,
	void        *serverData)
{
	ImageFilterHandler *h = (ImageFilterHandler *)local;
	XCALL_INIT;

	if (version < 1)
		return AFUNC_BADVERSION;

	h->create   = (void *)Create;
	h->destroy  = (void *)Destroy;
	h->load     = (void *)Load;
	h->save     = (void *)Save;
	h->copy     = (void *)Copy;
	h->process  = (void *)Process;
	h->flags    = (void *)Flags;
	h->descln   = 0;
	h->useItems = 0;
	h->changeID = 0;

	msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	if (!msg)
		return AFUNC_BADGLOBAL;

	return AFUNC_OK;
}

/* ----------------------------------------------------------------
 * Server description
 * ---------------------------------------------------------------- */

ServerRecord ServerDesc[] = {
	{ "ImageFilterHandler",   "LensFlare",
	  (ActivateFunc *)Activate },
	{ "ImageFilterInterface", "LensFlare",
	  (ActivateFunc *)Interface },
	{ 0 }
};
