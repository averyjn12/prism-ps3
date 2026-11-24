/* v0.28: the opening. A beam of white light crosses the dark, meets a glass prism and leaves it as
 * the app's six spectrum colours; PRISM writes itself in strokes (each letter its own colour, then
 * white), PLAYSTATION above it, the spectrum bar below, "by Bananafuzzbuddy" last, then it fades to
 * the app. Original drawing only: no Sony logo, typeface, sound or boot animation.
 * intro_draw(t) draws the frame at t seconds (the PC renderer calls it too); main.c runs it for
 * INTRO_LEN seconds of real time and stops at any button. */
#include <math.h>
#include <string.h>
#include "ui.h"
#include "look.h"
#include "intro.h"

static const unsigned char spec[6][3] = {
	{ 0x8F, 0x6B, 0xFF }, { 0x4F, 0xB6, 0xFF }, { 0x6B, 0xF0, 0xC2 }, { 0xF7, 0xE0, 0x6A }, { 0xFF, 0x8A, 0x5C }, { 0xFF, 0x5F, 0xA8 },
};

static float clamp01(float x)
{
	return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

/* 0 before a, 1 after b, smooth between */
static float ramp(float t, float a, float b)
{
	float x = clamp01((t - a) / (b - a));

	return x * x * (3.0f - 2.0f * x);
}

static col_t spec_col(int i, int a)
{
	return RGBA(spec[i][0], spec[i][1], spec[i][2], a);
}

static col_t mix(col_t p, col_t q, float k)
{
	int i, out[4];

	for (i = 0; i < 4; i++)
	{
		int x = (int)((p >> (24 - 8 * i)) & 0xFF), y = (int)((q >> (24 - 8 * i)) & 0xFF);

		out[i] = x + (int)((y - x) * k);
	}
	return RGBA(out[0], out[1], out[2], out[3]);
}

static col_t with_alpha(col_t c, int a)
{
	return (c & 0xFFFFFF00u) | (u32)(a & 0xFF);
}

/* a straight stroke w wide */
static void seg(float x0, float y0, float x1, float y1, float w, col_t c)
{
	float dx = x1 - x0, dy = y1 - y0, n = sqrtf(dx * dx + dy * dy), nx, ny;

	if (n < 0.01f)
		return;
	nx = -dy / n * w * 0.5f;
	ny = dx / n * w * 0.5f;
	quad(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x0 - nx, y0 - ny, x1 - nx, y1 - ny, c, c, c, c);
}

/* a soft band along a line: alpha a at the line, 0 at w/2 either side */
static void glow(float x0, float y0, float x1, float y1, float w, col_t c, int a)
{
	float dx = x1 - x0, dy = y1 - y0, n = sqrtf(dx * dx + dy * dy), nx, ny;
	col_t in = with_alpha(c, a), out = with_alpha(c, 0);

	if (n < 0.01f || a <= 0)
		return;
	nx = -dy / n * w * 0.5f;
	ny = dx / n * w * 0.5f;
	quad(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x0, y0, x1, y1, out, out, in, in);
	quad(x0, y0, x1, y1, x0 - nx, y0 - ny, x1 - nx, y1 - ny, in, in, out, out);
}

/* ---- the wordmark: each letter a polyline in a box 1 high (y down), drawn on by length ---- */

#define MAXP	40

typedef struct
{
	int n;
	float x[MAXP], y[MAXP];
} path_t;

static void arc(path_t *p, float cx, float cy, float r, float a0, float a1, int steps)
{
	int i;

	for (i = 0; i <= steps && p->n < MAXP; i++)
	{
		float a = (a0 + (a1 - a0) * i / steps) * 3.14159265f / 180.0f;

		p->x[p->n] = cx + r * cosf(a);
		p->y[p->n] = cy + r * sinf(a);
		p->n++;
	}
}

static void pt(path_t *p, float x, float y)
{
	if (p->n < MAXP)
	{
		p->x[p->n] = x;
		p->y[p->n] = y;
		p->n++;
	}
}

/* letter k of P R I S M: its paths (up to 2) and its width */
static int letter(int k, path_t *a, path_t *b, float *w)
{
	memset(a, 0, sizeof *a);
	memset(b, 0, sizeof *b);
	switch (k)
	{
	case 0:	/* P */
		pt(a, 0.0f, 1.0f); pt(a, 0.0f, 0.0f); pt(a, 0.40f, 0.0f);
		arc(a, 0.40f, 0.265f, 0.265f, -90.0f, 90.0f, 12);
		pt(a, 0.0f, 0.53f);
		*w = 0.68f;
		return 1;
	case 1:	/* R */
		pt(a, 0.0f, 1.0f); pt(a, 0.0f, 0.0f); pt(a, 0.40f, 0.0f);
		arc(a, 0.40f, 0.265f, 0.265f, -90.0f, 90.0f, 12);
		pt(a, 0.0f, 0.53f);
		pt(b, 0.32f, 0.53f); pt(b, 0.70f, 1.0f);
		*w = 0.70f;
		return 2;
	case 2:	/* I */
		pt(a, 0.0f, 0.0f); pt(a, 0.0f, 1.0f);
		*w = 0.0f;
		return 1;
	case 3:	/* S */
		arc(a, 0.33f, 0.25f, 0.25f, -20.0f, -270.0f, 14);
		arc(a, 0.33f, 0.75f, 0.25f, -90.0f, 160.0f, 14);
		*w = 0.66f;
		return 1;
	default:	/* M */
		pt(a, 0.0f, 1.0f); pt(a, 0.0f, 0.0f); pt(a, 0.42f, 0.64f); pt(a, 0.84f, 0.0f); pt(a, 0.84f, 1.0f);
		*w = 0.84f;
		return 1;
	}
}

static float path_len(const path_t *p, float sx, float sy)
{
	float L = 0.0f;
	int i;

	for (i = 1; i < p->n; i++)
	{
		float dx = (p->x[i] - p->x[i - 1]) * sx, dy = (p->y[i] - p->y[i - 1]) * sy;

		L += sqrtf(dx * dx + dy * dy);
	}
	return L;
}

/* the first `upto` (logical units of length) of a path, at (ox, oy), H high; soft = a glow band
 * (alpha c's, fading out to w/2) instead of a solid stroke with filled joints */
static void draw_path(const path_t *p, float ox, float oy, float H, float upto, float w, col_t c, int soft)
{
	int i;

	for (i = 1; i < p->n && upto > 0.0f; i++)
	{
		float x0 = ox + p->x[i - 1] * H, y0 = oy + p->y[i - 1] * H, x1 = ox + p->x[i] * H, y1 = oy + p->y[i] * H;
		float dx = x1 - x0, dy = y1 - y0, L = sqrtf(dx * dx + dy * dy);

		if (L > upto)
		{
			x1 = x0 + dx * upto / L;
			y1 = y0 + dy * upto / L;
		}
		if (soft)
			glow(x0, y0, x1, y1, w, c, (int)(c & 0xFF));
		else
		{
			seg(x0, y0, x1, y1, w, c);
			rect(x0 - w * 0.5f, y0 - w * 0.5f, x0 + w * 0.5f, y0 + w * 0.5f, c);	/* fills the joint */
		}
		upto -= L;
	}
}

#define WORD_H		56.0f
#define WORD_GAP	0.30f
#define WORD_TOP	252.0f
#define STROKE		7.0f

static void wordmark(float t)
{
	path_t a, b;
	float widths[5], total = 0.0f, x;
	int k;

	for (k = 0; k < 5; k++)
	{
		letter(k, &a, &b, &widths[k]);
		total += widths[k] * WORD_H + (k ? WORD_GAP * WORD_H : 0.0f);
	}
	x = 320.0f - total / 2.0f;
	for (k = 0; k < 5; k++)
	{
		float start = 1.60f + 0.12f * k, p = ramp(t, start, start + 0.45f), white = ramp(t, start + 0.45f, start + 0.95f);
		int np = letter(k, &a, &b, &widths[k]);
		float La = path_len(&a, WORD_H, WORD_H), Lb = np > 1 ? path_len(&b, WORD_H, WORD_H) : 0.0f, L = La + Lb, done = p * L;
		col_t c = mix(spec_col(k, 0x80), INK, white);

		if (p > 0.0f)
		{
			/* a soft halo in the letter's own colour while it draws, then a faint one */
			int ha = (int)(0x30 * (1.0f - 0.65f * white));

			draw_path(&a, x, WORD_TOP, WORD_H, done, STROKE + 16.0f, spec_col(k, ha), 1);
			if (np > 1 && done > La)
				draw_path(&b, x, WORD_TOP, WORD_H, done - La, STROKE + 16.0f, spec_col(k, ha), 1);
			draw_path(&a, x, WORD_TOP, WORD_H, done, STROKE, c, 0);
			if (np > 1 && done > La)
				draw_path(&b, x, WORD_TOP, WORD_H, done - La, STROKE, c, 0);
		}
		x += widths[k] * WORD_H + WORD_GAP * WORD_H;
	}
}

/* ---- the prism and the light ---- */

#define PTX	320.0f
#define PTY	70.0f
#define PLX	240.0f
#define PLY	208.0f
#define PRX	400.0f
#define PRY	208.0f

void intro_draw(float t)
{
	float in = ramp(t, 0.0f, 0.35f), fade = 1.0f - ramp(t, INTRO_LEN - 0.7f, INTRO_LEN);
	float ex = PTX + 0.56f * (PLX - PTX), ey = PTY + 0.56f * (PLY - PTY);	/* where the beam enters */
	float xx = PTX + 0.60f * (PRX - PTX), xy = PTY + 0.60f * (PRY - PTY);	/* where the colours leave */
	float beam = ramp(t, 0.20f, 0.90f), edges = ramp(t, 0.30f, 1.00f), inner = ramp(t, 0.88f, 1.10f);
	float rays = ramp(t, 1.05f, 1.75f), calm = 1.0f - 0.55f * ramp(t, 2.6f, 3.4f);
	int i;

	ui_clear(RGBA(0x02, 0x03, 0x0A, 0x80));
	/* a faint pool of light where the prism stands */
	quad(LX0, 0.0f, LX1, 0.0f, LX0, 448.0f, LX1, 448.0f,
		RGBA(0x10, 0x18, 0x40, (int)(0x30 * in)), RGBA(0x1C, 0x10, 0x3A, (int)(0x30 * in)),
		RGBA(0x02, 0x03, 0x0A, 0), RGBA(0x02, 0x03, 0x0A, 0));

	/* the beam, from the left edge to the prism's face */
	if (beam > 0.0f)
	{
		float sx = LX0 - 30.0f, sy = 240.0f, hx = sx + (ex - sx) * beam, hy = sy + (ey - sy) * beam;
		float dl = sqrtf((ex - sx) * (ex - sx) + (ey - sy) * (ey - sy)), ux = (ex - sx) / dl, uy = (ey - sy) / dl;

		glow(sx, sy, hx, hy, 26.0f, RGBA(0xC8, 0xD8, 0xFF, 0), (int)(0x34 * calm));
		seg(sx, sy, hx, hy, 2.4f, RGBA(0xF4, 0xF7, 0xFF, (int)(0x78 * calm)));
		if (beam < 1.0f)
			glow(hx - ux * 40.0f, hy - uy * 40.0f, hx + ux * 4.0f, hy + uy * 4.0f, 12.0f, RGBA(0xFF, 0xFF, 0xFF, 0), 0x60);
	}

	/* the glass: a faint body, then its three edges drawn on */
	if (edges > 0.0f)
	{
		int ga = (int)(0x22 * edges);
		float e1 = clamp01(edges * 3.0f), e2 = clamp01(edges * 3.0f - 1.0f), e3 = clamp01(edges * 3.0f - 2.0f);
		col_t ec = RGBA(0xD8, 0xE8, 0xFF, 0x70);

		tri(PTX, PTY, PLX, PLY, PRX, PRY, RGBA(0x9A, 0xC8, 0xFF, ga), RGBA(0x30, 0x40, 0x90, ga / 2), RGBA(0x50, 0x30, 0x90, ga / 2));
		seg(PTX, PTY, PTX + (PLX - PTX) * e1, PTY + (PLY - PTY) * e1, 2.0f, ec);
		seg(PLX, PLY, PLX + (PRX - PLX) * e2, PLY + (PRY - PLY) * e2, 2.0f, ec);
		seg(PRX, PRY, PRX + (PTX - PRX) * e3, PRY + (PTY - PRY) * e3, 2.0f, ec);
		glow(PTX, PTY, PTX + (PLX - PTX) * e1, PTY + (PLY - PTY) * e1, 10.0f, RGBA(0xB0, 0xD0, 0xFF, 0), 0x18);
		glow(PRX, PRY, PRX + (PTX - PRX) * e3, PRY + (PTY - PRY) * e3, 10.0f, RGBA(0xB0, 0xD0, 0xFF, 0), 0x18);
	}

	/* inside the glass the white light turns, a little brighter where it leaves */
	if (inner > 0.0f)
	{
		glow(ex, ey, xx, xy, 16.0f, RGBA(0xFF, 0xFF, 0xFF, 0), (int)(0x46 * inner * calm));
		seg(ex, ey, ex + (xx - ex) * inner, ey + (xy - ey) * inner, 2.0f, RGBA(0xFF, 0xFF, 0xFF, (int)(0x60 * calm)));
	}

	/* the colours, fanning out to the right edge */
	if (rays > 0.0f)
	{
		float far = xx + (LX1 + 10.0f - xx) * rays;

		for (i = 0; i < 6; i++)
		{
			float y0 = 118.0f + i * 23.0f, y1 = y0 + 23.5f, k = (far - xx) / (LX1 + 10.0f - xx);
			int a = (int)(0x50 * calm);

			/* a wedge from the exit point to the far edge, fading as it goes */
			tri(xx, xy, far, xy + (y0 - xy) * k, far, xy + (y1 - xy) * k, spec_col(5 - i, a), spec_col(5 - i, a / 3), spec_col(5 - i, a / 3));
		}
		glow(xx - 6.0f, xy, xx + 34.0f, xy + 4.0f, 10.0f, RGBA(0xFF, 0xFF, 0xFF, 0), (int)(0x48 * (1.0f - 0.5f * rays)));
	}

	wordmark(t);

	/* PLAYSTATION above, spaced out; the spectrum bar grows from the middle below */
	{
		float a = ramp(t, 2.2f, 2.6f), bar = ramp(t, 2.5f, 3.0f), by = ramp(t, 2.85f, 3.35f);

		if (a > 0.0f)
			text_centre(F_SMALL, 320, 222, 9, RGBA(0x8D, 0x96, 0xBD, (int)(0x80 * a)), "PLAYSTATION");
		if (bar > 0.0f)
			spectrum_bar(320.0f - 150.0f * bar, 320.0f + 150.0f * bar, 324.0f, 2.0f, 6, 0x80);
		if (by > 0.0f)
			text_centre(F_BODY, 320, 340, 1, RGBA(0xE9, 0xED, 0xFF, (int)(0x70 * by)), "by Bananafuzzbuddy");
	}

	/* fade in from black at the start, out to the app's dark at the end */
	if (in < 1.0f)
		rect(LX0, 0.0f, LX1, 448.0f, RGBA(0, 0, 0, (int)(0x80 * (1.0f - in))));
	if (fade < 1.0f)
		rect(LX0, 0.0f, LX1, 448.0f, RGBA(0x06, 0x0A, 0x1A, (int)(0x80 * (1.0f - fade))));
}
