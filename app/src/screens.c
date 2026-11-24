/* The launcher's look on the PS3 side: launcher/ui/main.c (GAMES, SEARCH, STARTING) and the HOME
 * menu of mcard.h, ported line for line onto ui.c. Changes from the PS2 program: four HOME
 * tiles (PS2, PS1, PS3, MEMORY CARD - PS1 and PS3 start from here now), the backdrop widened to
 * 16:9, time from the clock instead of frames. Text: DejaVu Sans. No Sony artwork, logos or
 * sounds. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ui.h"
#include "gfx.h"
#include "games.h"
#include "titles.h"
#include "screens.h"
#include "look.h"
#include "check.h"
#include "setup.h"
#include "icons.h"

#define QUERY_MAX	16

unsigned frame;
int screen = SCR_HOME, plat = P_PS2;
static int home_sel;
static short view[MAX_GAMES];
static int n_view, sel, sel_of[N_PLAT];
static float pos;
static char query[QUERY_MAX + 1];
static int kx, ky;
static char note[120];
static int note_frames;
static unsigned pan_t0;
static u32 *bg;			/* the backdrop's still part, drawn once */

static const char *const plat_name[N_PLAT] = { "PS1", "PS2", "PS3", "PSP" };

void ui_say(const char *s)
{
	strncpy(note, s, sizeof note - 1);
	note[sizeof note - 1] = 0;
	note_frames = 60 * 7;
}

/* ------------------------------------------------------------------ drawing helpers */

static const unsigned char spectrum[6][3] = {
	{ 0x8F, 0x6B, 0xFF }, { 0x4F, 0xB6, 0xFF }, { 0x6B, 0xF0, 0xC2 }, { 0xF7, 0xE0, 0x6A }, { 0xFF, 0x8A, 0x5C }, { 0xFF, 0x5F, 0xA8 },
};

void spectrum_bar(float x0, float x1, float y, float h, int stops, int alpha)
{
	int i;

	for (i = 0; i < stops - 1; i++)
	{
		float a = x0 + (x1 - x0) * i / (stops - 1), b = x0 + (x1 - x0) * (i + 1) / (stops - 1);

		hgrad(a, y, b, y + h, RGBA(spectrum[i][0], spectrum[i][1], spectrum[i][2], alpha),
			RGBA(spectrum[i + 1][0], spectrum[i + 1][1], spectrum[i + 1][2], alpha));
	}
}

float fsin(float t)
{
	t -= (float)(int)t;
	if (t < 0.0f)
		t += 1.0f;
	return t < 0.5f ? 16.0f * t * (0.5f - t) : -16.0f * (t - 0.5f) * (1.0f - t);
}

float fcos(float t)
{
	return fsin(t + 0.25f);
}

static col_t hue(float h, int a)
{
	float f;
	int i, j, r, g, b;

	h -= (float)(int)h;
	if (h < 0.0f)
		h += 1.0f;
	f = h * 6.0f;
	i = (int)f;
	if (i > 5)
		i = 5;
	f -= (float)i;
	j = (i + 1) % 6;
	r = (int)(spectrum[i][0] + (spectrum[j][0] - spectrum[i][0]) * f);
	g = (int)(spectrum[i][1] + (spectrum[j][1] - spectrum[i][1]) * f);
	b = (int)(spectrum[i][2] + (spectrum[j][2] - spectrum[i][2]) * f);
	return RGBA(r, g, b, a);
}

static const struct
{
	float x, y, r, ax, ay, px, py, pr, ph;
	int alpha;
} prisms[3] = {
	{ 96.0f, 330.0f, 150.0f, 70.0f, 46.0f, 47.0f, 61.0f, 170.0f, 0.10f, 0x1C },
	{ 548.0f, 104.0f, 112.0f, 58.0f, 40.0f, 53.0f, 37.0f, -140.0f, 0.55f, 0x16 },
	{ 430.0f, 372.0f, 128.0f, 84.0f, 34.0f, 71.0f, 43.0f, 210.0f, 0.80f, 0x12 },
};

enum { SH_CROSS, SH_SQUARE, SH_CIRCLE };

static const struct
{
	unsigned char kind, size;
	short x, y;
	signed char vx, vy, spin;
} floaters[14] = {
	{ SH_CROSS, 11, 60, 120, 26, -14, 31 },    { SH_SQUARE, 13, 200, 400, -18, -22, -43 }, { SH_CIRCLE, 9, 330, 70, 14, 19, 50 },
	{ SH_CROSS, 8, 520, 300, -22, -12, -27 },  { SH_SQUARE, 9, 610, 190, -30, 9, 37 },     { SH_CIRCLE, 14, 110, 250, 21, 12, 60 },
	{ SH_CROSS, 14, 420, 430, 12, -27, 53 },   { SH_SQUARE, 7, 20, 40, 33, 17, -29 },      { SH_CIRCLE, 7, 580, 410, -15, -25, 40 },
	{ SH_CROSS, 9, 260, 220, -11, 23, -35 },   { SH_SQUARE, 11, 470, 30, 17, 28, 47 },     { SH_CIRCLE, 11, 650, 90, -27, 15, 45 },
	{ SH_CROSS, 7, 160, 470, 19, -31, 33 },    { SH_CIRCLE, 8, 380, 330, -24, -9, 55 },
};

static void floater(int kind, float cx, float cy, float r, float turn, col_t c)
{
	int k, n = kind == SH_CIRCLE ? 12 : 4;
	float step = 1.0f / n;

	if (kind == SH_CROSS)
	{
		float dx = r * fcos(turn), dy = r * fsin(turn);

		line(cx - dx, cy - dy, cx + dx, cy + dy, c);
		line(cx + dy, cy - dx, cx - dy, cy + dx, c);
		return;
	}
	for (k = 0; k < n; k++)
	{
		float a0 = turn + step * k, a1 = a0 + step;

		line(cx + r * fcos(a0), cy + r * fsin(a0), cx + r * fcos(a1), cy + r * fsin(a1), c);
	}
}

void backdrop(void)
{
	float t = (float)frame / 60.0f, wrap = LX1 - LX0 + 60.0f;
	int i;

	if (!bg)
	{
		/* the still part once: the dark, and two soft lights, blue from the top left and violet
		 * from the bottom right (to the canvas's own edges; the PS2 program's 640 is narrower) */
		ui_clear(VOID);
		quad(LX0, 0.0f, 420.0f, 0.0f, LX0, 300.0f, 420.0f, 300.0f,
			RGBA(0x1A, 0x2A, 0x6A, 0x78), RGBA(0x1A, 0x2A, 0x6A, 0x00), RGBA(0x1A, 0x2A, 0x6A, 0x00), RGBA(0x1A, 0x2A, 0x6A, 0x00));
		quad(260.0f, 180.0f, LX1, 180.0f, 260.0f, 448.0f, LX1, 448.0f,
			RGBA(0x2A, 0x15, 0x52, 0x00), RGBA(0x2A, 0x15, 0x52, 0x00), RGBA(0x2A, 0x15, 0x52, 0x00), RGBA(0x2A, 0x15, 0x52, 0x78));
		bg = (u32 *)malloc(CW * CH * 4);
		if (bg)
			memcpy(bg, fb, CW * CH * 4);
	}
	else
	{
		u64 c0 = ui_clock ? ui_clock() : 0;

		memcpy(fb, bg, CW * CH * 4);
		if (ui_clock)
			ui_t_bgcopy = ui_clock() - c0;
	}

	for (i = 0; i < 3; i++)
	{
		float cx = prisms[i].x + prisms[i].ax * fsin(t / prisms[i].px + prisms[i].ph);
		float cy = prisms[i].y + prisms[i].ay * fsin(t / prisms[i].py + prisms[i].ph * 1.7f);
		float turn = t / prisms[i].pr + prisms[i].ph, r = prisms[i].r;
		float h = t / 80.0f + cx / 900.0f + cy / 1400.0f + prisms[i].ph;
		int a = prisms[i].alpha;

		tri(cx + r * fcos(turn), cy + r * fsin(turn), cx + r * fcos(turn + 0.3333f), cy + r * fsin(turn + 0.3333f),
			cx + r * fcos(turn + 0.6667f), cy + r * fsin(turn + 0.6667f), hue(h, a), hue(h + 0.10f, a * 2 / 5), hue(h + 0.20f, a * 3 / 4));
	}
	for (i = 0; i < (int)(sizeof floaters / sizeof floaters[0]); i++)
	{
		float x = floaters[i].x + floaters[i].vx * t / 10.0f + 9.0f * fsin(t / 13.0f + i * 0.21f);
		float y = floaters[i].y + floaters[i].vy * t / 10.0f + 7.0f * fsin(t / 17.0f + i * 0.37f);

		x -= wrap * (float)(int)(x / wrap);
		if (x < 0.0f)
			x += wrap;
		y -= 508.0f * (float)(int)(y / 508.0f);
		if (y < 0.0f)
			y += 508.0f;
		floater(floaters[i].kind, LX0 + x - 30.0f, y - 30.0f, (float)floaters[i].size, t / floaters[i].spin,
			hue(t / 60.0f + i * 0.13f + x / 1300.0f, 0x1E));
	}
}

void top_bar(const char *name)
{
	text_ex(F_BOLD, LEFT, 28, 1.0f, 6, INK, name);
	spectrum_bar(LEFT, RIGHT, 62.0f, 2.0f, 6, 0x80);
}

void top_right(const char *a, col_t ca, const char *b, col_t cb)
{
	int w = text_w(F_SMALL, a, 1) + text_w(F_SMALL, b, 1), x = RIGHT - w;

	x = text_ex(F_SMALL, x, 34, 1.0f, 1, ca, a);
	text_ex(F_SMALL, x, 34, 1.0f, 1, cb, b);
}

enum { IC_CROSS, IC_SQUARE, IC_TRIANGLE, IC_CIRCLE };

static void icon(int which, float x, float y)
{
	if (which == IC_CROSS)
	{
		stroke(x, y, x + 11.0f, y + 11.0f, INK);
		stroke(x + 11.0f, y, x, y + 11.0f, INK);
	}
	else if (which == IC_SQUARE)
		frame_rect(x, y, x + 12.0f, y + 12.0f, 2.0f, INK);
	else if (which == IC_CIRCLE)
	{
		static const float o[9][2] = { { 12.0f, 6.0f }, { 10.2f, 10.2f }, { 6.0f, 12.0f }, { 1.8f, 10.2f }, { 0.0f, 6.0f },
			{ 1.8f, 1.8f }, { 6.0f, 0.0f }, { 10.2f, 1.8f }, { 12.0f, 6.0f } };
		int k;

		for (k = 0; k < 8; k++)
			stroke(x + o[k][0], y + o[k][1], x + o[k + 1][0], y + o[k + 1][1], INK);
	}
	else
	{
		stroke(x + 6.0f, y, x + 12.0f, y + 11.0f, INK);
		stroke(x + 12.0f, y + 11.0f, x, y + 11.0f, INK);
		stroke(x, y + 11.0f, x + 6.0f, y, INK);
	}
}

int legend_item(int x, int which, const char *key, const char *label)
{
	if (which >= 0)
	{
		icon(which, (float)x, 407.0f);
		x += 19;
	}
	else
		x = text(F_SMALL, x, 404, INK, key) + 6;
	return text(F_SMALL, x, 404, DIM, label) + 20;
}

int legend_icon(int x, int which, const char *label)
{
	static const int map[4] = { IC_CROSS, IC_SQUARE, IC_TRIANGLE, IC_CIRCLE };

	return legend_item(x, map[which], NULL, label);
}

void legend_line(void)
{
	rect(LEFT, 396.0f, RIGHT, 397.0f, LINE);
}

/* the note line (AMBER), when there is one */
int note_up(void)
{
	return note_frames > 0;
}

const char *note_text(void)
{
	return note;
}

/* ------------------------------------------------------------------ jackets */

static const unsigned char tints[12][6] = {
	{ 0xE0, 0xA0, 0x3A, 0x7A, 0x2A, 0x4A }, { 0x2F, 0x9C, 0x8A, 0x0F, 0x33, 0x50 }, { 0x5A, 0x6A, 0xE0, 0x1A, 0x14, 0x50 },
	{ 0xC2, 0x52, 0x3A, 0x4A, 0x14, 0x20 }, { 0x6A, 0xA8, 0x3A, 0x14, 0x3A, 0x2A }, { 0xB0, 0x5A, 0xD0, 0x2A, 0x14, 0x50 },
	{ 0x3A, 0x9A, 0xD8, 0x10, 0x28, 0x50 }, { 0xD8, 0x6A, 0x8A, 0x50, 0x1A, 0x3A }, { 0xD8, 0xC0, 0x40, 0x5A, 0x3A, 0x10 },
	{ 0x40, 0xC0, 0xB0, 0x10, 0x40, 0x40 }, { 0x8A, 0x8A, 0xE8, 0x20, 0x20, 0x60 }, { 0xE0, 0x7A, 0x3A, 0x50, 0x1C, 0x10 },
};

static float ucos(int k)
{
	static const float q[7] = { 1.0f, 0.9659f, 0.8660f, 0.7071f, 0.5f, 0.2588f, 0.0f };

	k %= 24;
	if (k < 0)
		k += 24;
	if (k <= 6)
		return q[k];
	if (k <= 12)
		return -q[12 - k];
	if (k <= 18)
		return -q[k - 12];
	return q[24 - k];
}

void disc(float cx, float cy, float ro, float ri, int al)
{
	int k;

	for (k = 0; k < 24; k++)
	{
		float c0 = ucos(k), s0 = ucos(k - 6), c1 = ucos(k + 1), s1 = ucos(k - 5);
		int w = k % 12;
		col_t co = RGBA(0xFF, 0xFF, 0xFF, al * 0x16 / 0x80), cin = RGBA(0xFF, 0xFF, 0xFF, al * 0x0A / 0x80);

		if (w >= 1 && w <= 5)
		{
			co = RGBA(spectrum[w][0], spectrum[w][1], spectrum[w][2], al * 0x44 / 0x80);
			cin = RGBA(spectrum[w][0], spectrum[w][1], spectrum[w][2], al * 0x14 / 0x80);
		}
		quad(cx + ro * c0, cy + ro * s0, cx + ro * c1, cy + ro * s1, cx + ri * c0, cy + ri * s0, cx + ri * c1, cy + ri * s1, co, co, cin, cin);
	}
}

static void jacket(const game_t *g, float cx, float cy, float s, float a, int big)
{
	const unsigned char *t = tints[g->tint];
	float x0 = cx - s / 2, y0 = cy - s / 2, x1 = x0 + s, y1 = y0 + s;
	int al = (int)(a * 0x80);
	col_t ca = RGBA(t[0], t[1], t[2], al), cb = RGBA(t[3], t[4], t[5], al);
	col_t cm = RGBA((t[0] + t[3]) / 2, (t[1] + t[4]) / 2, (t[2] + t[5]) / 2, al);

	if (al <= 0)
		return;
	if (big)
	{
		float o = 6.0f, h = 22.0f;
		col_t g1 = RGBA(0x7F, 0xE3, 0xFF, 0x26), g0 = RGBA(0x7F, 0xE3, 0xFF, 0x00);

		vgrad(x0 - o, y0 - o - h, x1 + o, y0 - o, g0, g1);
		vgrad(x0 - o, y1 + o, x1 + o, y1 + o + h, g1, g0);
		hgrad(x0 - o - h, y0 - o, x0 - o, y1 + o, g0, g1);
		hgrad(x1 + o, y0 - o, x1 + o + h, y1 + o, g1, g0);
		quad(x0 - o - h, y0 - o - h, x0 - o, y0 - o - h, x0 - o - h, y0 - o, x0 - o, y0 - o, g0, g0, g0, g1);
		quad(x1 + o, y0 - o - h, x1 + o + h, y0 - o - h, x1 + o, y0 - o, x1 + o + h, y0 - o, g0, g0, g1, g0);
		quad(x0 - o - h, y1 + o, x0 - o, y1 + o, x0 - o - h, y1 + o + h, x0 - o, y1 + o + h, g0, g1, g0, g0);
		quad(x1 + o, y1 + o, x1 + o + h, y1 + o, x1 + o, y1 + o + h, x1 + o + h, y1 + o + h, g1, g0, g0, g0);
		frame_rect(x0 - o, y0 - o, x1 + o, y1 + o, 2.0f, GLOW);
	}

	if (g->cover)
	{
		sprite_tex(g->cover, x0, y0, 0.5f, 0.5f, x1, y1, 127.5f, 127.5f, RGBA(0x80, 0x80, 0x80, al));
		return;
	}

	quad(x0, y0, x1, y0, x0, y1, x1, y1, ca, cm, cm, cb);
	disc(x0 + s * 0.64f, y0 + s * 0.6f, s * 0.3f, s * 0.09f, al);
	tri(x0, y0, x0 + s * 0.7f, y0, x0, y0 + s * 0.62f, RGBA(0xFF, 0xFF, 0xFF, al * 0x24 / 0x80), RGBA(0xFF, 0xFF, 0xFF, 0), RGBA(0xFF, 0xFF, 0xFF, 0));
	text_ex(F_HEAD, (int)(x0 + s * 0.08f), (int)(y0 + s * 0.04f), s / 150.0f * 2.2f, 0, RGBA(0xFF, 0xFF, 0xFF, al * 0x6C / 0x80), g->init);
	if (big)
	{
		char ln[100];

		vgrad(x0, y1 - 46.0f, x1, y1, RGBA(0, 0, 0, 0), RGBA(0, 0, 0, 0x54));
		fit(ln, sizeof ln, F_SMALL, g->shown, (int)s - 18);
		text(F_SMALL, (int)x0 + 9, (int)y1 - 23, INK, ln);
	}
}

static float lerp(float a, float b, float t)
{
	return a + (b - a) * t;
}

static const float knot_x[5] = { 0.0f, 131.0f, 216.0f, 280.0f, 330.0f };
static const float knot_s[5] = { 150.0f, 84.0f, 58.0f, 40.0f, 30.0f };
static const float knot_a[5] = { 1.0f, 0.55f, 0.3f, 0.0f, 0.0f };

#define ROW_Y	150.0f

static void mirror(const game_t *g, float cx, float y, float s, float a)
{
	const unsigned char *t = tints[g->tint];
	float x0 = cx - s / 2, x1 = x0 + s, h = s * 0.22f;
	int al = (int)(a * 0x80 * 0.3f);

	if (g->cover)
		sprite_tex(g->cover, x0, y, 0.5f, 127.5f, x1, y + h, 127.5f, 128.0f * 0.78f, RGBA(0x80, 0x80, 0x80, al));
	else
		rect(x0, y, x1, y + h, RGBA(t[3], t[4], t[5], al));
	vgrad(x0, y, x1, y + h + 1.0f, RGBA(0x06, 0x0A, 0x1A, 0x10), RGBA(0x06, 0x0A, 0x1A, 0x80));
}

static const game_t *vg(int i)
{
	return &games[plat][view[i]];
}

static void carousel(void)
{
	int ring, side;

	if (pos != (float)sel)
	{
		pos += ((float)sel - pos) * 0.28f;
		if (pos - (float)sel < 0.01f && (float)sel - pos < 0.01f)
			pos = (float)sel;
	}
	for (ring = 3; ring >= 0; ring--)
	{
		for (side = -1; side <= 1; side += 2)
		{
			int base = (int)(pos + 0.5f), i = base + side * ring, k;
			float d, ad, t;

			if (ring == 0 && side == 1)
				continue;
			if (i < 0 || i >= n_view)
				continue;
			d = (float)i - pos;
			ad = d < 0 ? -d : d;
			if (ad >= 3.0f)
				continue;
			k = (int)ad;
			t = ad - k;
			{
				const game_t *g = vg(i);
				float cx = 320.0f + (d < 0 ? -1.0f : 1.0f) * lerp(knot_x[k], knot_x[k + 1], t);
				float s = lerp(knot_s[k], knot_s[k + 1], t), a = lerp(knot_a[k], knot_a[k + 1], t);
				int big = ad < 0.5f;

				mirror(g, cx, ROW_Y + s / 2 + (big ? 9.0f : 3.0f), s, a);
				jacket(g, cx, ROW_Y, s, a, big);
			}
		}
	}
}

static void size_text(char *out, unsigned n, u64 bytes)
{
	unsigned secs = (unsigned)(bytes >> 11);

	if (secs >= (1u << 19))
		snprintf(out, n, "%u.%u GB", secs >> 19, ((secs & ((1u << 19) - 1)) * 10) >> 19);
	else
		snprintf(out, n, "%u MB", (secs + 256) >> 9);
}

static void text_pan(int f, int x0, int x1, int y, col_t color, const char *s)
{
	const int hold = 100, per_px = 2;
	int over = text_w(f, s, 0) - (x1 - x0), t, off;

	if (over <= 0)
	{
		text_centre(f, (x0 + x1) / 2, y, 0, color, s);
		return;
	}
	t = (int)((frame - pan_t0) % (unsigned)(2 * (hold + over * per_px)));
	if (t < hold)
		off = 0;
	else if (t < hold + over * per_px)
		off = (t - hold) / per_px;
	else if (t < 2 * hold + over * per_px)
		off = over;
	else
		off = over - (t - 2 * hold - over * per_px) / per_px;
	scissor((float)x0, (float)x1, (float)y - 2.0f, (float)(y + font_h(f) + 4));
	text(f, x0 - off, y, color, s);
	scissor_reset();
}

static void letter_strip(void)
{
	unsigned char has[27];
	int i, cur = n_view ? vg(sel)->letter : -1;

	memset(has, 0, sizeof has);
	for (i = 0; i < n_view; i++)
		has[vg(i)->letter] = 1;
	for (i = 0; i < 27; i++)
	{
		char s[2] = { (char)(i ? 'A' + i - 1 : '#'), 0 };
		int cx = LEFT + 10 + i * (RIGHT - LEFT - 20) / 26;

		text_centre(i == cur ? F_BODY : F_SMALL, cx, i == cur ? 347 : 349, 0, i == cur ? GLOW : has[i] ? DIM : FAINT, s);
		if (i == cur)
			rect((float)cx - 7, 368.0f, (float)cx + 7, 370.0f, GLOW);
	}
}

static const char *kind_name(const game_t *g)
{
	if (g->kind >= 4)
		return plat == P_PS1 ? "PS1 DISC" : plat == P_PS2 ? "PS2 DISC" : "PS3 DISC";
	if (plat == P_PS1)
		return "PS1 CD";
	if (plat == P_PSP)
		return "PSP UMD";
	if (plat == P_PS3)
		return "PS3";
	return g->kind ? "CD" : "DVD";
}

/* ---- v0.15: a game's options (Square), and deleting it after two questions ---- */

enum { GM_BROWSE, GM_OPTIONS, GM_ASK1, GM_ASK2 };
enum { OPT_ADD_SC, OPT_DEL_SC, OPT_RIP, OPT_COPY, OPT_DELETE, OPT_CANCEL };
static const char *const opt_text[] = { "Add to this list", "Remove from this list", "Copy the disc to a storage device",
	"Copy to another device", "Delete", "Cancel" };
static int gm_mode, gm_pick, gm_n, gm_opt[6];
static unsigned gm_at;
static char gm_what[160];
#define ASK2_ARM	45		/* frames before the last "Delete" takes a press: no double-tap deletes */

static void gm_options(const game_t *g)
{
	gm_n = 0;
	if (g->kind >= 4)
	{
		gm_opt[gm_n++] = g->shortcut ? OPT_DEL_SC : OPT_ADD_SC;
		if (g->kind == 4 && plat != P_PSP)
			gm_opt[gm_n++] = OPT_RIP;	/* v0.38: PS1 and PS2 discs too (rip_image.h) */
	}
	else
	{
		/* v0.38: a PS2 game on the hard disk can be copied (to the card's exFAT part); the ones on
		 * the card stay where they are (Bananafuzzbuddy, 11-23: a PS2 game's options had no copy entry) */
		if (plat != P_PS2 || g->where == 2)
			gm_opt[gm_n++] = OPT_COPY;
		gm_opt[gm_n++] = OPT_DELETE;
	}
	gm_opt[gm_n++] = OPT_CANCEL;
	gm_pick = gm_n - 1;		/* starts on "Cancel" */
	gm_mode = GM_OPTIONS;
}

void ui_ask_delete(const char *what)
{
	snprintf(gm_what, sizeof gm_what, "%s", what);
	gm_mode = GM_ASK1;
	gm_pick = 0;			/* starts on "No" */
}

static void sheet(float x0, float y0, float x1, float y1)
{
	rect(LX0, 0.0f, LX1, 448.0f, RGBA(0x02, 0x04, 0x0C, 0x58));
	rect(x0, y0, x1, y1, RGBA(0x0D, 0x14, 0x33, 0x80));
	frame_rect(x0, y0, x1, y1, 1.0f, LINE);
	spectrum_bar(x0, x1, y0, 2.0f, 6, 0x80);
}

static void two_buttons(int y, int pick, const char *no, const char *yes, col_t yes_c)
{
	int c;

	for (c = 0; c < 2; c++)
	{
		float bx = c ? 330.0f : 200.0f;

		if (pick == c)
		{
			rect(bx, (float)y - 6.0f, bx + 110.0f, (float)y + 24.0f, RGBA(0x7F, 0xE3, 0xFF, 0x1C));
			frame_rect(bx, (float)y - 6.0f, bx + 110.0f, (float)y + 24.0f, 1.0f, GLOW);
		}
		text_centre(F_BODY, (int)bx + 55, y, 0, c ? yes_c : (pick == c ? INK : DIM), c ? yes : no);
	}
}

static void gm_overlay(const game_t *g)
{
	char ln[160];
	int k;

	if (gm_mode == GM_OPTIONS)
	{
		int y0 = 130, h = 52 + 36 * gm_n;

		sheet(146.0f, (float)y0, 494.0f, (float)(y0 + h));
		fit(ln, sizeof ln, F_SMALL, g->shown, 320);
		text_centre(F_SMALL, 320, y0 + 14, 1, DIM, ln);
		for (k = 0; k < gm_n; k++)
		{
			int y = y0 + 48 + 36 * k, on = gm_pick == k;

			if (on)
			{
				rect(166.0f, (float)y - 5.0f, 474.0f, (float)y + 23.0f, RGBA(0x7F, 0xE3, 0xFF, 0x1C));
				frame_rect(166.0f, (float)y - 5.0f, 474.0f, (float)y + 23.0f, 1.0f, GLOW);
			}
			text_centre(F_BODY, 320, y, 0, gm_opt[k] == OPT_DELETE ? AMBER : on ? INK : DIM, opt_text[gm_opt[k]]);
		}
	}
	else if (gm_mode == GM_ASK1 || gm_mode == GM_ASK2)
	{
		int last = gm_mode == GM_ASK2, armed = !last || frame - gm_at >= ASK2_ARM;

		sheet(120.0f, 128.0f, 520.0f, 312.0f);
		text_centre(F_BOLD, 320, 144, 0, INK, last ? "Delete it for good?" : "Delete this game?");
		fit(ln, sizeof ln, F_BODY, g->shown, 370);
		text_centre(F_BODY, 320, 180, 0, INK, ln);
		fit(ln, sizeof ln, F_SMALL, gm_what, 380);
		text_centre(F_SMALL, 320, 208, 0, DIM, ln);
		text_centre(F_SMALL, 320, 230, 0, AMBER, last ? "This is the last question. It cannot be undone." : "It cannot be brought back afterwards.");
		two_buttons(264, gm_pick, last ? "Keep it" : "No", last ? "Delete" : "Yes", last ? (armed ? AMBER : FAINT) : (gm_pick ? INK : DIM));
	}
}

static void draw_games(void)
{
	const game_t *g = vg(sel);
	char ln[110], size[24], head[24];
	int x;
	static const game_t *shown;

	snprintf(head, sizeof head, "%s GAMES", plat_name[plat]);
	top_bar(query[0] ? "SEARCH" : head);
	if (query[0])
	{
		snprintf(ln, sizeof ln, "\"%s\"", query);
		snprintf(size, sizeof size, " " DOT " %d OF %d", n_view, n_games[plat]);
		top_right(ln, GLOW, size, DIM);
	}
	else
	{
		snprintf(ln, sizeof ln, " " DOT " %d GAME%s " DOT " A-Z", n_games[plat], n_games[plat] == 1 ? "" : "S");
		top_right(plat == P_PS2 ? "SD CARD + HDD" : "ALL DEVICES", AMBER, ln, DIM);
	}
	carousel();
	if (g != shown)
	{
		shown = g;
		pan_t0 = frame;
	}
	text_pan(F_HEAD, LEFT, RIGHT, 270, INK, g->shown);
	size_text(size, sizeof size, g->size);
	if (plat == P_PS3 && g->kind == 3)
		snprintf(size, sizeof size, "FOLDER");
	if (g->kind >= 4)
		snprintf(size, sizeof size, "%s", g->kind == 4 ? "IN THE DRIVE" : "NOT IN THE DRIVE");
	if (plat != P_PS2 || g->kind < 4)
	{
		/* v0.44: every game says where it is, PS2 games on the card too (Bananafuzzbuddy, 11-23: the copy of
		 * Tiny Tees on the card showed "1 MB", the one on the hard disk "1 MB HDD") */
		char t[24];

		snprintf(t, sizeof t, "%s " DOT " %s", size, where_name(g->where));
		snprintf(size, sizeof size, "%s", t);
	}
	if (g->id[0])
		snprintf(ln, sizeof ln, "%s " DOT " %s " DOT " %s", g->id, kind_name(g), size);
	else
		snprintf(ln, sizeof ln, "%s " DOT " %s", kind_name(g), size);
	text_centre(F_SMALL, 320, 306, 1, DIM, ln);
	snprintf(ln, sizeof ln, "%d OF %d", sel + 1, n_view);
	text_centre(F_SMALL, 320, 325, 1, FAINT, ln);
	if (note_frames > 0)
		text_centre(F_SMALL, 320, 376, 0, AMBER, note);
	else
		letter_strip();
	legend_line();
	x = legend_item(LEFT, IC_CROSS, NULL, "Play");
	x = legend_item(x, IC_SQUARE, NULL, "Options");
	x = legend_item(x, IC_CIRCLE, NULL, "Menu");
	x = legend_item(x, IC_TRIANGLE, NULL, query[0] ? "Search again" : "Search");
	x = legend_item(x, -1, "L1 R1", "Letter");
	legend_item(x, -1, "SELECT", "Rescan");
	gm_overlay(g);
}

static const char *const key_rows[4] = { "ABCDEFGHI", "JKLMNOPQR", "STUVWXYZ0", "123456789" };
#define KEY_W	32
#define KEY_H	30
#define KEY_GAP	4
#define KEY_X	LEFT
#define KEY_Y	140

static void draw_search(void)
{
	char ln[100], rest[40];
	int r, c, x, i;

	top_bar("SEARCH");
	snprintf(ln, sizeof ln, "%d MATCH%s", n_view, n_view == 1 ? "" : "ES");
	snprintf(rest, sizeof rest, " " DOT " %d GAMES", n_games[plat]);
	top_right(ln, n_view ? GLOW : AMBER, rest, DIM);
	rect(LEFT, 84.0f, RIGHT, 124.0f, PANEL);
	frame_rect(LEFT, 84.0f, RIGHT, 124.0f, 1.0f, LINE);
	frame_rect(LEFT + 13.0f, 96.0f, LEFT + 24.0f, 107.0f, 2.0f, DIM);
	stroke(LEFT + 23.0f, 106.0f, LEFT + 29.0f, 112.0f, DIM);
	x = text_ex(F_BOLD, LEFT + 40, 93, 1.0f, 1, INK, query);
	if ((frame / 30) & 1)
		rect((float)x + 2, 93.0f, (float)x + 4, 115.0f, GLOW);
	if (!query[0])
		text(F_SMALL, LEFT + 52, 97, FAINT, "type part of a name");
	for (r = 0; r < 4; r++)
	{
		for (c = 0; c < 9; c++)
		{
			float kx0 = (float)(KEY_X + c * (KEY_W + KEY_GAP)), ky0 = (float)(KEY_Y + r * (KEY_H + KEY_GAP));
			char s[2] = { key_rows[r][c], 0 };
			int on = (r == ky && c == kx);

			rect(kx0, ky0, kx0 + KEY_W, ky0 + KEY_H, on ? GLOW : PANEL);
			text_centre(F_BODY, (int)kx0 + KEY_W / 2, (int)ky0 + 6, 0, on ? VOID : DIM, s);
		}
	}
	{
		float ky0 = (float)(KEY_Y + 4 * (KEY_H + KEY_GAP));
		float sx1 = (float)(KEY_X + 5 * (KEY_W + KEY_GAP) - KEY_GAP), dx1 = (float)(KEY_X + 9 * (KEY_W + KEY_GAP) - KEY_GAP);
		int on_sp = (ky == 4 && kx < 5), on_del = (ky == 4 && kx >= 5);

		rect((float)KEY_X, ky0, sx1, ky0 + KEY_H, on_sp ? GLOW : PANEL);
		text_centre(F_SMALL, (int)(KEY_X + sx1) / 2, (int)ky0 + 8, 1, on_sp ? VOID : DIM, "SPACE");
		rect(sx1 + KEY_GAP, ky0, dx1, ky0 + KEY_H, on_del ? GLOW : PANEL);
		text_centre(F_SMALL, (int)(sx1 + KEY_GAP + dx1) / 2, (int)ky0 + 8, 1, on_del ? VOID : DIM, "DELETE");
	}
	for (i = 0; i < 7 && i < n_view; i++)
	{
		float y = (float)(KEY_Y + i * 34);

		if (i == 0)
		{
			rect(374.0f, y, RIGHT, y + 30.0f, RGBA(0x7F, 0xE3, 0xFF, 0x16));
			rect(374.0f, y, 377.0f, y + 30.0f, GLOW);
		}
		fit(ln, sizeof ln, F_BODY, vg(i)->shown, RIGHT - 374 - 20);
		text(F_BODY, 384, (int)y + 6, i == 0 ? INK : DIM, ln);
	}
	if (n_view > 7)
	{
		snprintf(ln, sizeof ln, "and %d more", n_view - 7);
		text(F_SMALL, 384, KEY_Y + 7 * 34 + 2, FAINT, ln);
	}
	if (!n_view)
		text(F_BODY, 384, KEY_Y + 6, AMBER, "No game has that in its name");
	legend_line();
	x = legend_item(LEFT, IC_CROSS, NULL, "Type");
	x = legend_item(x, IC_SQUARE, NULL, "Delete");
	x = legend_item(x, -1, "START", "Show matches");
	legend_item(x, IC_TRIANGLE, NULL, "All games");
}

void draw_message(const char *head, const char *l1, const char *l2, const char *key, const char *label)
{
	int x;

	top_bar("PRISM");
	text_centre(F_HEAD, 320, 150, 0, INK, head);
	text_centre(F_BODY, 320, 206, 0, DIM, l1);
	text_centre(F_BODY, 320, 232, 0, DIM, l2);
	if (note_frames > 0)
		text_centre(F_SMALL, 320, 290, 0, AMBER, note);
	legend_line();
	x = legend_item(LEFT, -1, key, label);
	legend_item(x, IC_CIRCLE, NULL, "Menu");
}

void progress(const void *gv, const char *head, const char *title, float done, const char *what)
{
	const game_t *g = (const game_t *)gv;
	char ln[100];

	backdrop();
	top_bar("PRISM");
	if (g)
	{
		jacket(g, 320.0f, 152.0f, 96.0f, 1.0f, 0);
		frame_rect(266.0f, 98.0f, 374.0f, 206.0f, 2.0f, GLOW);
	}
	text_centre(F_SMALL, 320, 226, 5, DIM, head);
	fit(ln, sizeof ln, F_HEAD, title, RIGHT - LEFT);
	text_centre(F_HEAD, 320, 246, 0, INK, ln);
	rect(172.0f, 292.0f, 468.0f, 296.0f, LINE);
	spectrum_bar(172.0f, 172.0f + 296.0f * done, 292.0f, 4.0f, 4, 0x80);
	text_centre(F_SMALL, 320, 308, 0, DIM, what);
	gfx_present();
}

/* ------------------------------------------------------------------ HOME */

void card_shape(float cx, float cy, float w, float h, int al)
{
	col_t body = RGBA(0x2A, 0x36, 0x6E, al), edg = RGBA(0x7F, 0xE3, 0xFF, al * 0x60 / 0x80);

	rect(cx - w, cy - h, cx + w, cy + h, body);
	rect(cx - w * 0.72f, cy - h * 0.78f, cx + w * 0.72f, cy - h * 0.30f, RGBA(0xE9, 0xED, 0xFF, al * 0x30 / 0x80));
	rect(cx - w * 0.5f, cy + h * 0.55f, cx + w * 0.5f, cy + h * 0.80f, RGBA(0x06, 0x0A, 0x1A, al));
	frame_rect(cx - w, cy - h, cx + w, cy + h, 2.0f, edg);
	spectrum_bar(cx - w * 0.72f, cx + w * 0.72f, cy - h * 0.10f, 3.0f, 4, al);
}

#define TILES	6
#define T_MC	-1
#define T_SET	-2
static const int tile_plat[TILES] = { P_PS1, P_PS2, P_PSP, P_PS3, T_MC, T_SET };	/* v0.16: in release order */
static int home_ask, home_yes, home_asked;
static void enter_list(int p);

static void draw_home(void)
{
	static const char *const name[TILES] = { "PS1", "PS2", "PSP", "PS3", "MEMORY CARD", "SETTINGS" };
	static const int pic[4] = { ICON_PS1, ICON_PS2, ICON_PSP, ICON_PS3 };
	char sub[TILES][40], why[160];
	int i, ready = ps2_ready(why, sizeof why);

	for (i = 0; i < 4; i++)
	{
		int p = tile_plat[i], n = n_games[p];

		if (n)
			snprintf(sub[i], sizeof sub[i], "%d game%s", n, n == 1 ? "" : "s");
		else
			strcpy(sub[i], p == P_PS2 && card_state ? "Card not read" : "No games yet");
	}
	if (n_games[P_PS2] && !ready)
		strcpy(sub[1], chk.ps2_ok ? "Needs setup" : "Not on this console");
	strcpy(sub[4], "Your saves");
	strcpy(sub[5], !chk.ps2_ok ? "This console" : ready ? "All set" : "PS2 setup");

	top_bar("PRISM");
	text_centre(F_SMALL, 320, 92, 5, DIM, "WHERE TO?");
	for (i = 0; i < TILES; i++)
	{
		int chosen = i == home_sel, al = chosen ? 0x80 : 0x50;
		float cx = 320.0f + (i - 2.5f) * 118.0f, lift = chosen ? 6.0f * fsin(frame / 200.0f) - 6.0f : 0.0f;
		float cy = 208.0f + lift, hw = 54.0f;

		rect(cx - hw, cy - 86.0f, cx + hw, cy + 96.0f, chosen ? RGBA(0x14, 0x1E, 0x4A, 0x78) : RGBA(0x0D, 0x14, 0x33, 0x60));
		frame_rect(cx - hw, cy - 86.0f, cx + hw, cy + 96.0f, chosen ? 2.0f : 1.0f, chosen ? GLOW : LINE);
		if (chosen)
			spectrum_bar(cx - hw, cx + hw, cy + 94.0f, 2.0f, 4, 0x80);
		if (i == 4)
			card_shape(cx, cy - 26.0f, 24.0f, 34.0f, al);
		else
			console_icon(i == 5 ? ICON_GEAR : pic[i], cx, cy - 26.0f, 86.0f, i < 4 && !n_games[tile_plat[i]] ? al * 3 / 4 : al,
				(float)frame / 60.0f, chosen);
		if (i >= 4)
		{
			float w = text_w(F_BODY, name[i], 0) * 0.76f;

			text_ex(F_BODY, (int)(cx - w / 2), (int)cy + 39, 0.76f, 0, chosen ? INK : DIM, name[i]);
		}
		else
			text_centre(F_BOLD, (int)cx, (int)cy + 34, 4, chosen ? INK : DIM, name[i]);
		text_centre(F_SMALL, (int)cx, (int)cy + 64, 0, chosen ? DIM : FAINT, sub[i]);
	}
	if (note_frames > 0)
		text_centre(F_SMALL, 320, 352, 0, AMBER, note);
	legend_line();
	{
		int x = legend_item(LEFT, IC_CROSS, NULL, "Open");

		x = legend_item(x, IC_CIRCLE, NULL, "Quit");
		x = legend_item(x, IC_SQUARE, NULL, "Copy disc");
		legend_item(x, -1, "SELECT", "Rescan");
	}
	if (home_ask)
	{
		sheet(110.0f, 120.0f, 530.0f, 312.0f);
		text_centre(F_BOLD, 320, 136, 0, INK, "Set up PS2 play from the SD card?");
		text_centre(F_SMALL, 320, 170, 0, DIM, "It is needed once. Prism adds 6 lines at the END of");
		text_centre(F_SMALL, 320, 188, 0, DIM, "/dev_hdd0/onxmb.bat (webMAN runs it when the XMB comes up)");
		text_centre(F_SMALL, 320, 206, 0, DIM, "and copies its PS2 helper and launcher image to the hard disk.");
		text_centre(F_SMALL, 320, 228, 0, GLOW, "Your own lines stay as they are. Nothing goes to flash.");
		two_buttons(266, home_yes, "Not now", "Set up", home_yes ? INK : DIM);
	}
}

/* ------------------------------------------------------------------ lists */

static int fold(int c)
{
	return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static const char *sort_key(const char *s)
{
	if (fold((unsigned char)s[0]) == 'T' && fold((unsigned char)s[1]) == 'H' && fold((unsigned char)s[2]) == 'E' && s[3] == ' ' && s[4])
		return s + 4;
	return s;
}

static int name_cmp(const char *a, const char *b)
{
	a = sort_key(a);
	b = sort_key(b);
	for (; *a && fold((unsigned char)*a) == fold((unsigned char)*b); a++, b++)
		;
	return fold((unsigned char)*a) - fold((unsigned char)*b);
}

static int alnum(int c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static void describe(game_t *g)
{
	const unsigned char *s = (const unsigned char *)g->name;
	const char *dot = strrchr(g->name, '.'), *k, *p, *title = NULL;
	unsigned n = 0, end = dot ? (unsigned)(dot - g->name) : strlen(g->name), i, h = 0;
	int c;

	if (g->kind >= 3)
		end = strlen(g->name);	/* a PS3 folder, a disc, a shortcut: a title, no extension to cut */
	g->cover = art_find(g->id, &title);
	if (!title && g->id[0] && (g->kind >= 4 || (end == strlen(g->id) && !strncmp(g->name, g->id, end))))
	{
		/* v0.43: a disc, or an image named by its title ID alone (a copy made before Prism knew
		 * the title): the name from the title list */
		static char listed[100];

		if (title_lookup(g->id, listed, sizeof listed) == 0)
			title = listed;
	}
	if (title)
	{
		for (i = 0; title[i] && i < sizeof g->shown - 1 && i < 100; i++)
			g->shown[i] = (title[i] >= 32 && title[i] < 127) ? title[i] : '?';
		g->shown[i] = 0;
	}
	else
	{
		for (i = 0; i < end && n < sizeof g->shown - 2; i++)
		{
			if (s[i] < 0x80)
				g->shown[n++] = (s[i] >= 32 && s[i] < 127) ? (char)s[i] : '?';
			else if (s[i] == 0xC2 && i + 1 < end && (s[i + 1] == 0xAE || s[i + 1] == 0xA9))
				i++;		/* (R) (C) in a PS3 TITLE: left out */
			else if (s[i] == 0xE2 && i + 2 < end && s[i + 1] == 0x84 && s[i + 2] == 0xA2)
				i += 2;		/* (TM) */
			else if (s[i] >= 0xC0)
				g->shown[n++] = '?';
		}
		g->shown[n] = 0;
	}
	if (g->id[0])
	{
		/* v0.45: a copy Prism made is a file called "<Title> (<ID>)" - the ID keeps two discs of one
		 * name apart on the device. The screen shows the title alone; the ID is on the line under it
		 * (Bananafuzzbuddy, 11-23: "why do all these games say SLUS behind them?") */
		size_t ln = strlen(g->shown), il = strlen(g->id);

		if (ln > il + 3 && g->shown[ln - 1] == ')' && g->shown[ln - il - 2] == '(' && g->shown[ln - il - 3] == ' '
			&& !strncmp(g->shown + ln - il - 1, g->id, il))
			g->shown[ln - il - 3] = 0;
	}
	k = sort_key(g->shown);
	c = fold((unsigned char)k[0]);
	g->letter = (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 1) : 0;
	for (p = k; *p && !alnum((unsigned char)*p); p++)
		;
	g->init[0] = *p ? (char)fold((unsigned char)*p) : '?';
	g->init[1] = 0;
	g->init[2] = 0;
	if (*p)
	{
		const char *q = strchr(p, ' ');

		while (q && *q && !alnum((unsigned char)*q))
			q++;
		if (q && *q)
			g->init[1] = (char)fold((unsigned char)*q);
		else if (alnum((unsigned char)p[1]))
			g->init[1] = (char)fold((unsigned char)p[1]);
	}
	for (i = 0; g->shown[i]; i++)
		h = h * 31 + (unsigned char)fold((unsigned char)g->shown[i]);
	g->tint = (unsigned char)(h % 12);
}

static int cmp_games(const void *a, const void *b)
{
	const game_t *x = (const game_t *)a, *y = (const game_t *)b;

	/* v0.34: the disc in the drive first (Bananafuzzbuddy, 11-23), then the rest by name */
	if ((x->kind == 4) != (y->kind == 4))
		return x->kind == 4 ? -1 : 1;
	return name_cmp(x->shown, y->shown);
}

static int matches(const char *name, const char *q)
{
	for (; *name; name++)
	{
		const char *a = name, *b = q;

		while (*b && fold((unsigned char)*a) == fold((unsigned char)*b))
		{
			a++;
			b++;
		}
		if (!*b)
			return 1;
	}
	return !*q;
}

static void filter(int keep)
{
	int i;

	n_view = 0;
	sel = 0;
	for (i = 0; i < n_games[plat]; i++)
	{
		if (matches(games[plat][i].shown, query))
		{
			if (i == keep)
				sel = n_view;
			view[n_view++] = (short)i;
		}
	}
	pos = (float)sel;
}

void ui_prepare(void)
{
	int p, i;

	for (p = 0; p < N_PLAT; p++)
	{
		for (i = 0; i < n_games[p]; i++)
			describe(&games[p][i]);
		qsort(games[p], n_games[p], sizeof(game_t), cmp_games);
		if (sel_of[p] >= n_games[p] || (n_games[p] && games[p][0].kind == 4))
			sel_of[p] = 0;	/* v0.34: a disc in the drive is where the list opens */
	}
	query[0] = 0;
	if (screen != SCR_HOME && screen != SCR_MC)
	{
		filter(sel_of[plat]);
		if (!n_view)
			screen = SCR_GAMES;
	}
}

static void jump_letter(int dir)
{
	int cur = vg(sel)->letter, i;

	if (dir > 0)
	{
		for (i = sel + 1; i < n_view && vg(i)->letter == cur; i++)
			;
		sel = i < n_view ? i : 0;
	}
	else
	{
		for (i = sel; i > 0 && vg(i - 1)->letter == cur; i--)
			;
		if (i == sel)
		{
			i = sel > 0 ? sel - 1 : n_view - 1;
			cur = vg(i)->letter;
			for (; i > 0 && vg(i - 1)->letter == cur; i--)
				;
		}
		sel = i;
	}
}

/* ------------------------------------------------------------------ the frame and the pad */

/* v0.11: per-phase frame times for the console's perf log (main.c sets ui_clock) */
u64 (*ui_clock)(void);
u64 ui_t_bgcopy, ui_t_backdrop;

void ui_draw(void)
{
	u64 t0 = ui_clock ? ui_clock() : 0;

	backdrop();
	if (ui_clock)
		ui_t_backdrop = ui_clock() - t0;
	if (screen == SCR_HOME)
		draw_home();
	else if (screen == SCR_MC)
		mc_draw();
	else if (screen == SCR_SETTINGS)
		settings_draw();
	else if (!n_games[plat])
	{
		if (plat == P_PSP)
			draw_message("No PSP games", "PSP games go in /PSPISO on the hard disk, a USB drive or the SD card", "(.iso or .cso). They start through the PSP launcher.", "SELECT", "Look again");
		else if (plat == P_PS1)
			draw_message("No PS1 games", "PS1 games go in /PSXISO on the SD card, the hard disk or a USB", "drive (.bin with its .cue).", "SELECT", "Look again");
		else if (plat == P_PS3)
			draw_message("No PS3 games", "PS3 games go in /GAMES (folders) or /PS3ISO on the hard disk or a", "USB drive. A disc in the drive: Square on the menu copies it.", "SELECT", "Look again");
		else if (card_state)
		{
			char l2[80];

			snprintf(l2, sizeof l2, "Is the SD card in the console's own slot? (code %d)", card_state);
			draw_message("Card not readable", "The card reader or the card could not be read.", l2, "SELECT", "Look again");
		}
		else
			draw_message("No PS2 games", "PS2 games go in /PS2ISO or /PS2PRISM on the card", "(.iso / .bin, or split .iso.0, .iso.1 ...).", "SELECT", "Look again");
	}
	else if (screen == SCR_SEARCH)
		draw_search();
	else
		draw_games();
	if (note_frames > 0)
		note_frames--;
}

const void *ui_chosen(void)
{
	return n_view ? vg(sel) : NULL;
}

static void search_pad(unsigned pressed)
{
	unsigned len = strlen(query);
	int before = n_view ? view[0] : -1;

	if (pressed & PAD_LEFT)
		kx = kx > 0 ? kx - 1 : 8;
	if (pressed & PAD_RIGHT)
		kx = kx < 8 ? kx + 1 : 0;
	if (pressed & PAD_UP)
		ky = ky > 0 ? ky - 1 : 4;
	if (pressed & PAD_DOWN)
		ky = ky < 4 ? ky + 1 : 0;
	if (pressed & PAD_CROSS)
	{
		if (ky == 4 && kx >= 5)
			pressed |= PAD_SQUARE;
		else if (len < QUERY_MAX)
		{
			char c = ky == 4 ? ' ' : key_rows[ky][kx];

			if (c != ' ' || len)
			{
				query[len] = c;
				query[len + 1] = 0;
			}
		}
	}
	if ((pressed & PAD_SQUARE) && len)
		query[len - 1] = 0;
	if (pressed & (PAD_CROSS | PAD_SQUARE))
		filter(before);
	if (pressed & PAD_TRIANGLE)
	{
		query[0] = 0;
		filter(-1);
		screen = SCR_GAMES;
	}
	else if (pressed & PAD_START)
	{
		if (n_view)
		{
			sel = 0;
			pos = 0.0f;
			screen = SCR_GAMES;
		}
		else
			ui_say("No game has that in its name.");
	}
}

static void enter_list(int p)
{
	plat = p;
	query[0] = 0;
	gm_mode = GM_BROWSE;
	filter(-1);
	sel = sel_of[plat] < n_view ? sel_of[plat] : 0;
	pos = (float)sel;
	screen = SCR_GAMES;
}

void ui_home_go(int tile)	/* the PC renderer's way to a tile: 0 PS1 1 PS2 2 PSP 3 PS3 4 MC 5 SETTINGS */
{
	screen = SCR_HOME;
	home_sel = tile;
}

void ui_open_settings(void)
{
	screen = SCR_SETTINGS;
	settings_open();
}

int ui_pad(unsigned pressed, unsigned held)
{
	(void)held;
	if (screen == SCR_HOME && home_ask)
	{
		if (pressed & (PAD_LEFT | PAD_RIGHT))
			home_yes ^= 1;
		if (pressed & PAD_CIRCLE)
			home_ask = 0;
		else if (pressed & PAD_CROSS)
		{
			home_ask = 0;
			if (home_yes)
				return DO_SETUP;
			enter_list(P_PS2);
		}
		return DO_NOTHING;
	}
	if (screen == SCR_HOME)
	{
		if (pressed & PAD_LEFT)
			home_sel = home_sel ? home_sel - 1 : TILES - 1;
		if (pressed & PAD_RIGHT)
			home_sel = home_sel < TILES - 1 ? home_sel + 1 : 0;
		if (pressed & PAD_SELECT)
			return DO_RESCAN;
		if (pressed & PAD_SQUARE)
			return DO_RIP;
		if (pressed & PAD_CIRCLE)
			return DO_QUIT;
		if (pressed & PAD_CROSS)
		{
			if (tile_plat[home_sel] == T_MC)
			{
				screen = SCR_MC;
				mc_open();
			}
			else if (tile_plat[home_sel] == T_SET)
				ui_open_settings();
			else if (tile_plat[home_sel] == P_PS2 && chk.ps2_ok && su.state == SU_NONE && su.shipped && !home_asked)
			{
				home_ask = 1;
				home_asked = 1;
				home_yes = 0;	/* starts on "Not now" */
			}
			else
				enter_list(tile_plat[home_sel]);
		}
		return DO_NOTHING;
	}
	if (screen == SCR_SETTINGS)
	{
		int r = settings_pad(pressed);

		if (r < 0)
		{
			screen = SCR_HOME;
			return DO_NOTHING;
		}
		return r;
	}
	if (screen == SCR_MC)
	{
		if (mc_pad(pressed))
			screen = SCR_HOME;
		return DO_NOTHING;
	}
	if (screen == SCR_SEARCH)
	{
		search_pad(pressed);
		return DO_NOTHING;
	}
	if (gm_mode == GM_OPTIONS)
	{
		if (pressed & PAD_UP)
			gm_pick = gm_pick ? gm_pick - 1 : gm_n - 1;
		if (pressed & PAD_DOWN)
			gm_pick = gm_pick < gm_n - 1 ? gm_pick + 1 : 0;
		if (pressed & (PAD_CIRCLE | PAD_TRIANGLE | PAD_SQUARE))
			gm_mode = GM_BROWSE;
		else if (pressed & PAD_CROSS)
		{
			static const int act[] = { DO_SC_ADD, DO_SC_DEL, DO_RIP, DO_COPYGAME, DO_DELETE_ASK, DO_NOTHING };

			gm_mode = GM_BROWSE;
			sel_of[plat] = n_view ? view[sel] : 0;
			return act[gm_opt[gm_pick]];
		}
		return DO_NOTHING;
	}
	if (gm_mode == GM_ASK1 || gm_mode == GM_ASK2)
	{
		if (pressed & (PAD_LEFT | PAD_RIGHT))
			gm_pick ^= 1;
		if (pressed & (PAD_CIRCLE | PAD_TRIANGLE))
			gm_mode = GM_BROWSE;
		else if (pressed & PAD_CROSS)
		{
			if (!gm_pick)
				gm_mode = GM_BROWSE;
			else if (gm_mode == GM_ASK1)
			{
				gm_mode = GM_ASK2;
				gm_pick = 0;	/* the last question starts on "Keep it" too */
				gm_at = frame;
			}
			else if (frame - gm_at >= ASK2_ARM)
			{
				gm_mode = GM_BROWSE;
				sel_of[plat] = n_view ? view[sel] : 0;
				return DO_DELETE;
			}
		}
		return DO_NOTHING;
	}
	if (pressed & PAD_CIRCLE)
	{
		if (!query[0])
			sel_of[plat] = n_view ? view[sel] : 0;
		screen = SCR_HOME;
		return DO_NOTHING;
	}
	if (pressed & PAD_SELECT)
		return DO_RESCAN;
	if (!n_view)
		return DO_NOTHING;
	if (pressed & PAD_LEFT)
		sel = sel > 0 ? sel - 1 : n_view - 1;
	if (pressed & PAD_RIGHT)
		sel = sel < n_view - 1 ? sel + 1 : 0;
	if (pressed & PAD_L1)
		jump_letter(-1);
	if (pressed & PAD_R1)
		jump_letter(1);
	if (pressed & PAD_SQUARE)
		gm_options(vg(sel));
	else if (pressed & PAD_TRIANGLE)
	{
		kx = ky = 0;
		screen = SCR_SEARCH;
	}
	else if (pressed & PAD_CROSS)
	{
		sel_of[plat] = view[sel];
		return DO_START;
	}
	return DO_NOTHING;
}
