/* prismui - the Prism launcher's PS2 program (S36 LR-MENU, 2026-11-21).
 *
 * Boot file of the "launcher" title made by bench/lr_title.py. Under the PS3's PS2 emulator
 * with the Prism stage2 loaded, reads of this title's command file PRISM.CMD are answered by
 * the stage2 (tree/ps2emu_stage2/prism/lr.h, "LR-MENU"):
 *
 *   sector 0            state text (PRISMCAT1, card=, count=, pin=, seq=, act=, last= ...)
 *   sectors 1..16       the games in /PS2ISO on the SD card, 128-byte records
 *   sector 64 + 32*k    action k: 0..255 make game k the disc, 256..511 queue a shortcut
 *                       title for game k-256, 512 look for the card again
 *
 * Screens (drawn first on the Claude Design canvas "Prism Launcher screens"):
 *   GAMES    the collection, A to Z, one row of jackets with the chosen one large; a jacket
 *            is drawn here from the game's name (no cover art files yet); an A-Z strip shows
 *            where you are; L1 / R1 jump a letter
 *   SEARCH   an on-screen keyboard; the list of matches updates as you type
 *   STARTING the disc switch and hand-over
 *
 * CROSS asks for the disc switch, restarts the PS2 side's disc modules and boots the game the
 * way a disc launcher does. SQUARE asks for a shortcut title when the console is set up for it.
 *
 * It is also the boot file of every shortcut title (the template). There it only runs when the
 * stage2 could not serve the game (no card, no file): it finds no PRISM.CMD and says so.
 *
 * -DDEMO: for an emulator on a PC (no stage2, nobody at the pad). The pad is played by a
 * fixed script so that screenshots show each screen.
 *
 * Text: DejaVu Sans (free licence), see gen_font.py. No Sony artwork, logos or sounds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <kernel.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <loadfile.h>
#include <fileio.h>
#include <io_common.h>
#include <libpad.h>
#include <gsKit.h>
#include <dmaKit.h>

#include "font_data.h"

#define SECTOR		2048
#define CMD_FILE	"cdrom0:\\PRISM.CMD;1"
#define CNF_FILE	"cdrom0:\\SYSTEM.CNF;1"
#define OWN_BOOT	"SLUS_999.1"	/* .10 the launcher, .11 a shortcut's own disc */
#define MENU_CAT	1
#define CAT_MAX		256
#define MENU_ACT	64
#define MENU_STEP	32
#define ACT_PIN		256
#define ACT_RESCAN	512

#define E_NOMEDIUM	(-7)
#define E_NOFILE	(-8)
#define E_PINKIT	(-20)
#define E_PINNAME	(-21)
#define E_PINFULL	(-22)
#define E_PINNED	(-23)

#define F_SMALL		0
#define F_BODY		1
#define F_BOLD		2
#define F_HEAD		3
#define DOT		"\x7f"		/* the middle dot in the font */

#define LEFT		38
#define RIGHT		602
#define QUERY_MAX	16

typedef struct
{
	char name[120];		/* as on the card (UTF-8) */
	char shown[100];	/* what the screen shows */
	unsigned secs;
	unsigned char idx;	/* its number at the stage2 */
	unsigned char on_hdd;	/* it has a shortcut title on the XMB */
	unsigned char queued;	/* asked for this session: 1 = add the shortcut, 2 = remove it */
	unsigned char letter;	/* 0 = '#', 1..26 = A..Z */
	unsigned char tint;
	char init[3];
	char id[12];		/* title ID from the game's own disc image, or empty */
	short art;		/* its cover's slot in the art file, -1 none */
	unsigned char kind;	/* 0 DVD, 1 CD, 2 CD as a raw 2352-byte image */
} game_t;

static struct
{
	int ok;			/* 1 = a stage2 answered */
	int card, count, more, pin, pins, seq, act, last, art;
	char ver[24];
} st;

static game_t games[CAT_MAX];	/* A to Z */
static int n_games;
static short view[CAT_MAX];	/* the games on screen: all of them, or the matches of a search */
static int n_view, sel;
static float pos;		/* where the row is; glides to sel */
static char query[QUERY_MAX + 1];
static int kx, ky;		/* the key under the cursor on the search screen */
static int cmd_fd = -1;
static unsigned char secbuf[8 * SECTOR] __attribute__((aligned(64)));
static char padbuf[2][256] __attribute__((aligned(64)));

static GSGLOBAL *gs;
static GSTEXTURE ftex;
static unsigned frame;
static char note[120];
static int note_frames;

enum { SCR_GAMES, SCR_SEARCH, SCR_NOCARD, SCR_NOSTAGE2, SCR_NOCMD };
static int screen;

/* ------------------------------------------------------------------ drawing */

#define RGBA(r, g, b, a)	GS_SETREG_RGBAQ(r, g, b, a, 0x00)
#define INK		RGBA(0xE9, 0xED, 0xFF, 0x80)
#define DIM		RGBA(0x8D, 0x96, 0xBD, 0x80)
#define FAINT		RGBA(0x4A, 0x54, 0x82, 0x80)
#define GLOW		RGBA(0x7F, 0xE3, 0xFF, 0x80)
#define AMBER		RGBA(0xFF, 0xC4, 0x6B, 0x80)
#define LINE		RGBA(0x25, 0x30, 0x5F, 0x80)
#define PANEL		RGBA(0x0D, 0x14, 0x33, 0x74)
#define VOID		RGBA(0x06, 0x0A, 0x1A, 0x80)

static void font_init(void)
{
	u32 *px;
	int i;

	ftex.Width = FONT_TEX_W;
	ftex.Height = FONT_TEX_H;
	ftex.PSM = GS_PSM_CT32;
	ftex.Filter = GS_FILTER_NEAREST;
	ftex.Delayed = 1;
	px = memalign(128, FONT_TEX_W * FONT_TEX_H * 4);
	for (i = 0; i < FONT_TEX_W * FONT_TEX_H; i++)
	{
		unsigned c = (font_cov[i >> 1] >> ((i & 1) * 4)) & 15;

		/* white, alpha = coverage (0x80 is opaque on the GS) */
		px[i] = 0x00808080u | ((c * 0x80 / 15) << 24);
	}
	ftex.Mem = px;
}

static const glyph_t *glyph(int f, unsigned c)
{
	if (c < FONT_FIRST || c > FONT_LAST)
		c = '?';

	return &font_glyphs[f][c - FONT_FIRST];
}

static int text_w(int f, const char *s, int track)
{
	int w = 0;

	for (; *s; s++)
		w += glyph(f, (unsigned char)*s)->adv + track;

	return w;
}

/* y is the top of the line; track = extra pixels after each character; scale 1 is the drawn size */
static int text_ex(int f, int x, int y, float scale, int track, u64 color, const char *s)
{
	float fx = (float)x;

	ftex.Filter = (scale == 1.0f) ? GS_FILTER_NEAREST : GS_FILTER_LINEAR;
	for (; *s; s++)
	{
		const glyph_t *g = glyph(f, (unsigned char)*s);

		if (g->w)
		{
			float x0 = fx + (g->ox - 1) * scale, y0 = (float)y + (g->oy - 1) * scale;

			/* One empty texel is drawn all round each glyph (the atlas keeps two between
			 * glyphs), and coordinates sit on texel CENTRES. Console run P2 (2026-11-21):
			 * with the sprite exactly the glyph's size and coordinates on texel edges, the
			 * bottom of every character was cut off on the console (each row came from the
			 * row above), while a PC emulator drew it whole. The two do not agree on where a
			 * pixel samples; with a spare row and column on every side, neither convention can
			 * cut a glyph, it can only sit one pixel off. */
			gsKit_prim_sprite_texture(gs, &ftex, x0, y0, g->u - 0.5f, g->v - 0.5f, x0 + (g->w + 2) * scale, y0 + (g->h + 2) * scale,
				g->u + g->w + 1.5f, g->v + g->h + 1.5f, 2, color);
		}
		fx += (g->adv + track) * scale;
	}

	return (int)fx;
}

static int text(int f, int x, int y, u64 color, const char *s)
{
	return text_ex(f, x, y, 1.0f, 0, color, s);
}

static void text_centre(int f, int cx, int y, int track, u64 color, const char *s)
{
	text_ex(f, cx - (text_w(f, s, track) - track) / 2, y, 1.0f, track, color, s);
}

static unsigned pan_t0;		/* the frame the chosen game last changed */

/* A line that may be too long for its place between x0 and x1: centred if it fits; otherwise it
 * waits, slides slowly to its end, waits, and slides back, so that all of it can be read. */
static void text_pan(int f, int x0, int x1, int y, u64 color, const char *s)
{
	const int hold = 100, per_px = 2;	/* frames waited at each end; frames per pixel moved */
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

	gsKit_set_scissor(gs, GS_SETREG_SCISSOR(x0, x1, y - 2, y + font_height[f] + 4));
	text(f, x0 - off, y, color, s);
	gsKit_set_scissor(gs, GS_SCISSOR_RESET);
}

/* Copy s, cut with "..." so that it is no wider than w. */
static void fit(char *out, unsigned n, int f, const char *s, int w)
{
	unsigned len;

	strncpy(out, s, n - 1);
	out[n - 1] = 0;
	len = strlen(out);
	while (len > 4 && text_w(f, out, 0) > w)
	{
		len--;
		strcpy(out + len - 3, "...");
	}
}

static void rect(float x0, float y0, float x1, float y1, u64 c)
{
	gsKit_prim_quad_gouraud(gs, x0, y0, x1, y0, x0, y1, x1, y1, 1, c, c, c, c);
}

/* left colour ca, right colour cb */
static void hgrad(float x0, float y0, float x1, float y1, u64 ca, u64 cb)
{
	gsKit_prim_quad_gouraud(gs, x0, y0, x1, y0, x0, y1, x1, y1, 1, ca, cb, ca, cb);
}

static void vgrad(float x0, float y0, float x1, float y1, u64 ca, u64 cb)
{
	gsKit_prim_quad_gouraud(gs, x0, y0, x1, y0, x0, y1, x1, y1, 1, ca, ca, cb, cb);
}

static void frame_rect(float x0, float y0, float x1, float y1, float t, u64 c)
{
	rect(x0, y0, x1, y0 + t, c);
	rect(x0, y1 - t, x1, y1, c);
	rect(x0, y0 + t, x0 + t, y1 - t, c);
	rect(x1 - t, y0 + t, x1, y1 - t, c);
}

/* a line two pixels thick (one-pixel lines shimmer on an interlaced screen) */
static void stroke(float x0, float y0, float x1, float y1, u64 c)
{
	gsKit_prim_line(gs, x0, y0, x1, y1, 2, c);
	gsKit_prim_line(gs, x0, y0 + 1.0f, x1, y1 + 1.0f, 2, c);
	gsKit_prim_line(gs, x0 + 1.0f, y0, x1 + 1.0f, y1, 2, c);
}

static const unsigned char spectrum[6][3] = {
	{ 0x8F, 0x6B, 0xFF }, { 0x4F, 0xB6, 0xFF }, { 0x6B, 0xF0, 0xC2 }, { 0xF7, 0xE0, 0x6A }, { 0xFF, 0x8A, 0x5C }, { 0xFF, 0x5F, 0xA8 },
};

/* stops = how many of the six colours to run through */
static void spectrum_bar(float x0, float x1, float y, float h, int stops, int alpha)
{
	int i;

	for (i = 0; i < stops - 1; i++)
	{
		float a = x0 + (x1 - x0) * i / (stops - 1), b = x0 + (x1 - x0) * (i + 1) / (stops - 1);

		hgrad(a, y, b, y + h, RGBA(spectrum[i][0], spectrum[i][1], spectrum[i][2], alpha),
			RGBA(spectrum[i + 1][0], spectrum[i + 1][1], spectrum[i + 1][2], alpha));
	}
}

/* ---- the moving backdrop ---- */

/* sine of t turns (t = 1 is a full circle), by two parabolas: smooth enough for drifting shapes */
static float fsin(float t)
{
	t -= (float)(int)t;
	if (t < 0.0f)
		t += 1.0f;

	return t < 0.5f ? 16.0f * t * (0.5f - t) : -16.0f * (t - 0.5f) * (1.0f - t);
}

static float fcos(float t)
{
	return fsin(t + 0.25f);
}

/* a colour on the spectrum's ring: h in turns, alpha a */
static u64 hue(float h, int a)
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

/* The prisms: three large solid triangles. Each wanders on its own slow loop, turns, and takes
 * its colour from the time and from where it is, a different shade at each corner. */
static const struct
{
	float x, y, r;		/* home and size */
	float ax, ay;		/* how far it wanders */
	float px, py, pr;	/* seconds per loop across, per loop up and down, per turn */
	float ph;		/* where in its loops it starts */
	int alpha;
} prisms[3] = {
	{ 96.0f, 330.0f, 150.0f, 70.0f, 46.0f, 47.0f, 61.0f, 170.0f, 0.10f, 0x1C },
	{ 548.0f, 104.0f, 112.0f, 58.0f, 40.0f, 53.0f, 37.0f, -140.0f, 0.55f, 0x16 },
	{ 430.0f, 372.0f, 128.0f, 84.0f, 34.0f, 71.0f, 43.0f, 210.0f, 0.80f, 0x12 },
};

/* The small ones: crosses, squares and circles that float across and turn as they go. */
enum { SH_CROSS, SH_SQUARE, SH_CIRCLE };

static const struct
{
	unsigned char kind, size;
	short x, y;		/* where it starts */
	signed char vx, vy;	/* pixels per ten seconds */
	signed char spin;	/* seconds per turn; the sign is the direction */
} floaters[14] = {
	{ SH_CROSS, 11, 60, 120, 26, -14, 31 },    { SH_SQUARE, 13, 200, 400, -18, -22, -43 }, { SH_CIRCLE, 9, 330, 70, 14, 19, 50 },
	{ SH_CROSS, 8, 520, 300, -22, -12, -27 },  { SH_SQUARE, 9, 610, 190, -30, 9, 37 },     { SH_CIRCLE, 14, 110, 250, 21, 12, 60 },
	{ SH_CROSS, 14, 420, 430, 12, -27, 53 },   { SH_SQUARE, 7, 20, 40, 33, 17, -29 },      { SH_CIRCLE, 7, 580, 410, -15, -25, 40 },
	{ SH_CROSS, 9, 260, 220, -11, 23, -35 },   { SH_SQUARE, 11, 470, 30, 17, 28, 47 },     { SH_CIRCLE, 11, 650, 90, -27, 15, 45 },
	{ SH_CROSS, 7, 160, 470, 19, -31, 33 },    { SH_CIRCLE, 8, 380, 330, -24, -9, 55 },
};

/* a line two pixels thick, for shapes behind everything else */
static void bline(float x0, float y0, float x1, float y1, u64 c)
{
	gsKit_prim_line(gs, x0, y0, x1, y1, 0, c);
	gsKit_prim_line(gs, x0, y0 + 1.0f, x1, y1 + 1.0f, 0, c);
}

static void floater(int kind, float cx, float cy, float r, float turn, u64 c)
{
	int k, n = kind == SH_CIRCLE ? 12 : 4;
	float step = 1.0f / n;

	if (kind == SH_CROSS)
	{
		float dx = r * fcos(turn), dy = r * fsin(turn);

		bline(cx - dx, cy - dy, cx + dx, cy + dy, c);
		bline(cx + dy, cy - dx, cx - dy, cy + dx, c);
		return;
	}

	for (k = 0; k < n; k++)
	{
		float a0 = turn + step * k, a1 = a0 + step;

		bline(cx + r * fcos(a0), cy + r * fsin(a0), cx + r * fcos(a1), cy + r * fsin(a1), c);
	}
}

static void backdrop(void)
{
	float t = (float)frame / 60.0f;		/* seconds */
	int i;

	gsKit_clear(gs, VOID);
	/* two soft lights: blue from the top left, violet from the bottom right */
	gsKit_prim_quad_gouraud(gs, 0.0f, 0.0f, 420.0f, 0.0f, 0.0f, 300.0f, 420.0f, 300.0f, 0,
		RGBA(0x1A, 0x2A, 0x6A, 0x78), RGBA(0x1A, 0x2A, 0x6A, 0x00), RGBA(0x1A, 0x2A, 0x6A, 0x00), RGBA(0x1A, 0x2A, 0x6A, 0x00));
	gsKit_prim_quad_gouraud(gs, 260.0f, 180.0f, 640.0f, 180.0f, 260.0f, 448.0f, 640.0f, 448.0f, 0,
		RGBA(0x2A, 0x15, 0x52, 0x00), RGBA(0x2A, 0x15, 0x52, 0x00), RGBA(0x2A, 0x15, 0x52, 0x00), RGBA(0x2A, 0x15, 0x52, 0x78));

	for (i = 0; i < 3; i++)
	{
		float cx = prisms[i].x + prisms[i].ax * fsin(t / prisms[i].px + prisms[i].ph);
		float cy = prisms[i].y + prisms[i].ay * fsin(t / prisms[i].py + prisms[i].ph * 1.7f);
		float turn = t / prisms[i].pr + prisms[i].ph, r = prisms[i].r;
		/* colour: one slow lap of the spectrum every 80 seconds, shifted by where it has got to */
		float h = t / 80.0f + cx / 900.0f + cy / 1400.0f + prisms[i].ph;
		int a = prisms[i].alpha;

		gsKit_prim_triangle_gouraud(gs, cx + r * fcos(turn), cy + r * fsin(turn),
			cx + r * fcos(turn + 0.3333f), cy + r * fsin(turn + 0.3333f),
			cx + r * fcos(turn + 0.6667f), cy + r * fsin(turn + 0.6667f), 0,
			hue(h, a), hue(h + 0.10f, a * 2 / 5), hue(h + 0.20f, a * 3 / 4));
	}

	for (i = 0; i < (int)(sizeof floaters / sizeof floaters[0]); i++)
	{
		/* across the screen and round again, with a slow sway */
		float x = floaters[i].x + floaters[i].vx * t / 10.0f + 9.0f * fsin(t / 13.0f + i * 0.21f);
		float y = floaters[i].y + floaters[i].vy * t / 10.0f + 7.0f * fsin(t / 17.0f + i * 0.37f);

		x -= 700.0f * (float)(int)(x / 700.0f);
		if (x < 0.0f)
			x += 700.0f;
		y -= 508.0f * (float)(int)(y / 508.0f);
		if (y < 0.0f)
			y += 508.0f;
		floater(floaters[i].kind, x - 30.0f, y - 30.0f, (float)floaters[i].size, t / floaters[i].spin,
			hue(t / 60.0f + i * 0.13f + x / 1300.0f, 0x1E));
	}
}

static void top_bar(const char *name)
{
	text_ex(F_BOLD, LEFT, 28, 1.0f, 6, INK, name);
	spectrum_bar(LEFT, RIGHT, 62.0f, 2.0f, 6, 0x80);
}

/* right-aligned run of pieces on the top bar's line: text, colour, ..., NULL */
static void top_right(const char *a, u64 ca, const char *b, u64 cb)
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

/* One entry of the legend: an icon (which >= 0) or a word in ink, then its label. Returns the
 * x of the next entry. */
static int legend_item(int x, int which, const char *key, const char *label)
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

static void legend_line(void)
{
	rect(LEFT, 396.0f, RIGHT, 397.0f, LINE);
}

/* ---- jackets: drawn from the name ---- */

static const unsigned char tints[12][6] = {
	{ 0xE0, 0xA0, 0x3A, 0x7A, 0x2A, 0x4A }, { 0x2F, 0x9C, 0x8A, 0x0F, 0x33, 0x50 }, { 0x5A, 0x6A, 0xE0, 0x1A, 0x14, 0x50 },
	{ 0xC2, 0x52, 0x3A, 0x4A, 0x14, 0x20 }, { 0x6A, 0xA8, 0x3A, 0x14, 0x3A, 0x2A }, { 0xB0, 0x5A, 0xD0, 0x2A, 0x14, 0x50 },
	{ 0x3A, 0x9A, 0xD8, 0x10, 0x28, 0x50 }, { 0xD8, 0x6A, 0x8A, 0x50, 0x1A, 0x3A }, { 0xD8, 0xC0, 0x40, 0x5A, 0x3A, 0x10 },
	{ 0x40, 0xC0, 0xB0, 0x10, 0x40, 0x40 }, { 0x8A, 0x8A, 0xE8, 0x20, 0x20, 0x60 }, { 0xE0, 0x7A, 0x3A, 0x50, 0x1C, 0x10 },
};

/* ---- covers: real artwork, when the console has an art file (bench/lr_art.py) ---- */

#define MENU_ART	20480
#define COVER		128
#define COVER_BYTES	(COVER * COVER * 4)
#define COVER_SECS	(COVER_BYTES / SECTOR)
#define N_COVERS	9

typedef struct
{
	GSTEXTURE tex;
	int game;		/* whose cover is loaded, -1 none */
	unsigned used;		/* the frame it was last drawn in */
} cover_t;

static cover_t covers[N_COVERS];
static int art_data;		/* the first cover's sector in the art file; 0 = no art */
static int want_cover = -1;	/* a game whose cover should be read next */

static void covers_init(void)
{
	int i;

	for (i = 0; i < N_COVERS; i++)
	{
		GSTEXTURE *t = &covers[i].tex;

		t->Width = COVER;
		t->Height = COVER;
		t->PSM = GS_PSM_CT32;
		t->Filter = GS_FILTER_LINEAR;
		t->Delayed = 1;
		t->Mem = memalign(128, COVER_BYTES);
		covers[i].game = -1;
	}
}

static void covers_forget(void)
{
	int i;

	for (i = 0; i < N_COVERS; i++)
		covers[i].game = -1;
	want_cover = -1;
}

/* The loaded cover of a game, or NULL; a game that has one and is not loaded gets asked for. */
static GSTEXTURE *cover_of(int gi)
{
	int i;

	if (games[gi].art < 0 || !art_data)
		return NULL;
	for (i = 0; i < N_COVERS; i++)
	{
		if (covers[i].game == gi)
		{
			covers[i].used = frame;
			return &covers[i].tex;
		}
	}
	want_cover = gi;

	return NULL;
}

/* Between frames: read ONE wanted cover, into a free slot or the one unseen for longest. */
static void cover_load(void)
{
	int i, best = 0, gi = want_cover;

	want_cover = -1;
	if (gi < 0 || cmd_fd < 0)
		return;
	for (i = 1; i < N_COVERS; i++)
	{
		if (covers[best].game >= 0 && (covers[i].game < 0 || covers[i].used < covers[best].used))
			best = i;
	}

	if (fioLseek(cmd_fd, (MENU_ART + art_data + games[gi].art * COVER_SECS) * SECTOR, SEEK_SET) >= 0
		&& fioRead(cmd_fd, covers[best].tex.Mem, COVER_BYTES) == COVER_BYTES)
	{
		covers[best].game = gi;
		covers[best].used = frame;
		gsKit_TexManager_invalidate(gs, &covers[best].tex);
	}
	else
	{
		covers[best].game = -1;
		games[gi].art = -1;	/* unreadable: its drawn jacket from now on */
	}
}

/* cosine of k * 15 degrees */
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

/* A disc on a drawn jacket: a pale ring with two wedges of spectrum, as a disc throws light. */
static void disc(float cx, float cy, float ro, float ri, int al)
{
	int k;

	for (k = 0; k < 24; k++)
	{
		float c0 = ucos(k), s0 = ucos(k - 6), c1 = ucos(k + 1), s1 = ucos(k - 5);
		int w = k % 12;		/* the two wedges sit opposite each other */
		u64 co = RGBA(0xFF, 0xFF, 0xFF, al * 0x16 / 0x80), ci = RGBA(0xFF, 0xFF, 0xFF, al * 0x0A / 0x80);

		if (w >= 1 && w <= 5)
		{
			co = RGBA(spectrum[w][0], spectrum[w][1], spectrum[w][2], al * 0x44 / 0x80);
			ci = RGBA(spectrum[w][0], spectrum[w][1], spectrum[w][2], al * 0x14 / 0x80);
		}
		gsKit_prim_quad_gouraud(gs, cx + ro * c0, cy + ro * s0, cx + ro * c1, cy + ro * s1, cx + ri * c0, cy + ri * s0,
			cx + ri * c1, cy + ri * s1, 1, co, co, ci, ci);
	}
}

/* centre (cx, cy), side s, a = 0..1 how solid, big = the chosen one */
static void jacket(const game_t *g, float cx, float cy, float s, float a, int big)
{
	const unsigned char *t = tints[g->tint];
	float x0 = cx - s / 2, y0 = cy - s / 2, x1 = x0 + s, y1 = y0 + s;
	int al = (int)(a * 0x80);
	u64 ca = RGBA(t[0], t[1], t[2], al), cb = RGBA(t[3], t[4], t[5], al);
	u64 cm = RGBA((t[0] + t[3]) / 2, (t[1] + t[4]) / 2, (t[2] + t[5]) / 2, al);
	GSTEXTURE *cv = cover_of((int)(g - games));

	if (big)
	{
		/* glow, then the frame */
		float o = 6.0f, h = 22.0f;
		u64 g1 = RGBA(0x7F, 0xE3, 0xFF, 0x26), g0 = RGBA(0x7F, 0xE3, 0xFF, 0x00);

		vgrad(x0 - o, y0 - o - h, x1 + o, y0 - o, g0, g1);
		vgrad(x0 - o, y1 + o, x1 + o, y1 + o + h, g1, g0);
		hgrad(x0 - o - h, y0 - o, x0 - o, y1 + o, g0, g1);
		hgrad(x1 + o, y0 - o, x1 + o + h, y1 + o, g1, g0);
		/* the four corners of the glow: bright only at the corner that touches the frame */
		gsKit_prim_quad_gouraud(gs, x0 - o - h, y0 - o - h, x0 - o, y0 - o - h, x0 - o - h, y0 - o, x0 - o, y0 - o, 1, g0, g0, g0, g1);
		gsKit_prim_quad_gouraud(gs, x1 + o, y0 - o - h, x1 + o + h, y0 - o - h, x1 + o, y0 - o, x1 + o + h, y0 - o, 1, g0, g0, g1, g0);
		gsKit_prim_quad_gouraud(gs, x0 - o - h, y1 + o, x0 - o, y1 + o, x0 - o - h, y1 + o + h, x0 - o, y1 + o + h, 1, g0, g1, g0, g0);
		gsKit_prim_quad_gouraud(gs, x1 + o, y1 + o, x1 + o + h, y1 + o, x1 + o, y1 + o + h, x1 + o + h, y1 + o + h, 1, g1, g0, g0, g0);
		frame_rect(x0 - o, y0 - o, x1 + o, y1 + o, 2.0f, GLOW);
	}

	if (cv)
	{
		/* the real cover */
		gsKit_TexManager_bind(gs, cv);
		gsKit_prim_sprite_texture(gs, cv, x0, y0, 0.5f, 0.5f, x1, y1, COVER - 0.5f, COVER - 0.5f, 1, RGBA(0x80, 0x80, 0x80, al));
		return;
	}

	/* no cover for this one: a sleeve in its own colour, a disc, its initials */
	gsKit_prim_quad_gouraud(gs, x0, y0, x1, y0, x0, y1, x1, y1, 1, ca, cm, cm, cb);
	disc(x0 + s * 0.64f, y0 + s * 0.6f, s * 0.3f, s * 0.09f, al);
	/* the facet: light across the top left corner */
	gsKit_prim_triangle_gouraud(gs, x0, y0, x0 + s * 0.7f, y0, x0, y0 + s * 0.62f, 1,
		RGBA(0xFF, 0xFF, 0xFF, al * 0x24 / 0x80), RGBA(0xFF, 0xFF, 0xFF, 0), RGBA(0xFF, 0xFF, 0xFF, 0));

	text_ex(F_HEAD, (int)(x0 + s * 0.08f), (int)(y0 + s * 0.04f), s / 150.0f * 2.2f, 0, RGBA(0xFF, 0xFF, 0xFF, al * 0x6C / 0x80), g->init);

	if (big)
	{
		char line[100];

		vgrad(x0, y1 - 46.0f, x1, y1, RGBA(0, 0, 0, 0), RGBA(0, 0, 0, 0x54));
		fit(line, sizeof line, F_SMALL, g->shown, (int)s - 18);
		text(F_SMALL, (int)x0 + 9, (int)y1 - 23, INK, line);
	}
}

static float lerp(float a, float b, float t)
{
	return a + (b - a) * t;
}

/* where a jacket sits and how big it is, d steps from the middle (0, 1, 2, 3) */
static const float knot_x[5] = { 0.0f, 131.0f, 216.0f, 280.0f, 330.0f };
static const float knot_s[5] = { 150.0f, 84.0f, 58.0f, 40.0f, 30.0f };
static const float knot_a[5] = { 1.0f, 0.55f, 0.3f, 0.0f, 0.0f };

#define ROW_Y	150.0f		/* the row's centre line */

/* What a jacket throws on the floor under it: its lower edge upside down, fading out. */
static void mirror(const game_t *g, float cx, float y, float s, float a)
{
	const unsigned char *t = tints[g->tint];
	GSTEXTURE *cv = cover_of((int)(g - games));
	float x0 = cx - s / 2, x1 = x0 + s, h = s * 0.22f;
	int al = (int)(a * 0x80 * 0.3f);

	if (cv)
	{
		gsKit_TexManager_bind(gs, cv);
		gsKit_prim_sprite_texture(gs, cv, x0, y, 0.5f, COVER - 0.5f, x1, y + h, COVER - 0.5f, COVER * 0.78f, 1, RGBA(0x80, 0x80, 0x80, al));
	}
	else
		rect(x0, y, x1, y + h, RGBA(t[3], t[4], t[5], al));
	/* fade it into the floor */
	vgrad(x0, y, x1, y + h + 1.0f, RGBA(0x06, 0x0A, 0x1A, 0x10), RGBA(0x06, 0x0A, 0x1A, 0x80));
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

	/* far ones first, the middle one last */
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
				const game_t *g = &games[view[i]];
				float cx = 320.0f + (d < 0 ? -1.0f : 1.0f) * lerp(knot_x[k], knot_x[k + 1], t);
				float s = lerp(knot_s[k], knot_s[k + 1], t), a = lerp(knot_a[k], knot_a[k + 1], t);
				int big = ad < 0.5f;

				mirror(g, cx, ROW_Y + s / 2 + (big ? 9.0f : 3.0f), s, a);
				jacket(g, cx, ROW_Y, s, a, big);
			}
		}
	}
}

static void size_text(char *out, unsigned n, unsigned secs)
{
	if (secs >= (1u << 19))
		snprintf(out, n, "%u.%u GB", secs >> 19, ((secs & ((1u << 19) - 1)) * 10) >> 19);
	else
		snprintf(out, n, "%u MB", (secs + 256) >> 9);
}

static void letter_strip(void)
{
	unsigned char has[27];
	int i, cur = n_view ? games[view[sel]].letter : -1;

	memset(has, 0, sizeof has);
	for (i = 0; i < n_view; i++)
		has[games[view[i]].letter] = 1;

	for (i = 0; i < 27; i++)
	{
		char s[2] = { (char)(i ? 'A' + i - 1 : '#'), 0 };
		int cx = LEFT + 10 + i * (RIGHT - LEFT - 20) / 26;

		text_centre(i == cur ? F_BODY : F_SMALL, cx, i == cur ? 347 : 349, 0, i == cur ? GLOW : has[i] ? DIM : FAINT, s);
		if (i == cur)
			rect((float)cx - 7, 368.0f, (float)cx + 7, 370.0f, GLOW);
	}
}

static void draw_games(void)
{
	const game_t *g = &games[view[sel]];
	char line[100], size[24];
	int x;

	top_bar(query[0] ? "SEARCH" : "GAMES");
	if (query[0])
	{
		snprintf(line, sizeof line, "\"%s\"", query);
		snprintf(size, sizeof size, " " DOT " %d OF %d", n_view, n_games);
		top_right(line, GLOW, size, DIM);
	}
	else
	{
		snprintf(line, sizeof line, " " DOT " %d GAME%s " DOT " A-Z", n_games, n_games == 1 ? "" : "S");
		top_right("SD CARD", AMBER, line, DIM);
	}

	carousel();

	{
		static const game_t *shown;

		if (g != shown)
		{
			shown = g;
			pan_t0 = frame;
		}
	}
	text_pan(F_HEAD, LEFT, RIGHT, 270, INK, g->shown);

	size_text(size, sizeof size, g->secs);
	if (g->id[0])
		snprintf(line, sizeof line, "%s " DOT " %s " DOT " %s", g->id, g->kind ? "CD" : "DVD", size);
	else
		snprintf(line, sizeof line, "%s " DOT " %s", g->kind ? "CD" : "DVD", size);
	if (g->on_hdd || g->queued)
	{
		const char *tag = g->queued == 1 ? "ADDED TO XMB" : g->queued == 2 ? "LEAVING XMB" : "ON XMB";
		int w;

		strcat(line, " " DOT " ");
		w = text_w(F_SMALL, line, 1) + text_w(F_SMALL, tag, 1);
		x = text_ex(F_SMALL, 320 - w / 2, 306, 1.0f, 1, DIM, line);
		text_ex(F_SMALL, x, 306, 1.0f, 1, AMBER, tag);
	}
	else
		text_centre(F_SMALL, 320, 306, 1, DIM, line);
	snprintf(line, sizeof line, "%d OF %d", sel + 1, n_view);
	text_centre(F_SMALL, 320, 325, 1, FAINT, line);

	if (note_frames > 0)
		text_centre(F_SMALL, 320, 376, 0, AMBER, note);
	else
		letter_strip();

	legend_line();
	x = legend_item(LEFT, IC_CROSS, NULL, "Play");
	x = legend_item(x, IC_CIRCLE, NULL, "Menu");
	x = legend_item(x, IC_TRIANGLE, NULL, query[0] ? "Search again" : "Search");
	if (st.pin)
		x = legend_item(x, IC_SQUARE, NULL, g->on_hdd ? "Remove from XMB" : "Add to XMB");
	x = legend_item(x, -1, "L1 R1", "Letter");
	if (!st.pin)
		legend_item(x, -1, "SELECT", "Rescan");
}

/* ---- search ---- */

static const char *const key_rows[4] = { "ABCDEFGHI", "JKLMNOPQR", "STUVWXYZ0", "123456789" };
#define KEY_W	32
#define KEY_H	30
#define KEY_GAP	4
#define KEY_X	LEFT
#define KEY_Y	140

static void draw_search(void)
{
	char line[100];
	int r, c, x, i;

	top_bar("SEARCH");
	snprintf(line, sizeof line, "%d MATCH%s", n_view, n_view == 1 ? "" : "ES");
	{
		char rest[40];

		snprintf(rest, sizeof rest, " " DOT " %d GAMES", n_games);
		top_right(line, n_view ? GLOW : AMBER, rest, DIM);
	}

	/* the field */
	rect(LEFT, 84.0f, RIGHT, 124.0f, PANEL);
	frame_rect(LEFT, 84.0f, RIGHT, 124.0f, 1.0f, LINE);
	frame_rect(LEFT + 13.0f, 96.0f, LEFT + 24.0f, 107.0f, 2.0f, DIM);	/* a small lens */
	stroke(LEFT + 23.0f, 106.0f, LEFT + 29.0f, 112.0f, DIM);
	x = text_ex(F_BOLD, LEFT + 40, 93, 1.0f, 1, INK, query);
	if ((frame / 30) & 1)
		rect((float)x + 2, 93.0f, (float)x + 4, 115.0f, GLOW);
	if (!query[0])
		text(F_SMALL, LEFT + 52, 97, FAINT, "type part of a name");

	/* the keys */
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

	/* the matches, as you type */
	for (i = 0; i < 7 && i < n_view; i++)
	{
		float y = (float)(KEY_Y + i * 34);

		if (i == 0)
		{
			rect(374.0f, y, RIGHT, y + 30.0f, RGBA(0x7F, 0xE3, 0xFF, 0x16));
			rect(374.0f, y, 377.0f, y + 30.0f, GLOW);
		}
		fit(line, sizeof line, F_BODY, games[view[i]].shown, RIGHT - 374 - 20);
		text(F_BODY, 384, (int)y + 6, i == 0 ? INK : DIM, line);
	}
	if (n_view > 7)
	{
		snprintf(line, sizeof line, "and %d more", n_view - 7);
		text(F_SMALL, 384, KEY_Y + 7 * 34 + 2, FAINT, line);
	}
	if (!n_view)
		text(F_BODY, 384, KEY_Y + 6, AMBER, "No game has that in its name");

	legend_line();
	x = legend_item(LEFT, IC_CROSS, NULL, "Type");
	x = legend_item(x, IC_SQUARE, NULL, "Delete");
	x = legend_item(x, -1, "START", "Show matches");
	legend_item(x, IC_TRIANGLE, NULL, "All games");
}

static void draw_message(const char *head, const char *l1, const char *l2, const char *key, const char *label)
{
	top_bar("PRISM");
	text_centre(F_HEAD, 320, 150, 0, INK, head);
	text_centre(F_BODY, 320, 206, 0, DIM, l1);
	text_centre(F_BODY, 320, 232, 0, DIM, l2);
	if (note_frames > 0)
		text_centre(F_SMALL, 320, 290, 0, AMBER, note);
	legend_line();
	legend_item(LEFT, -1, key, label);
	if (st.ver[0])
		text(F_SMALL, RIGHT - text_w(F_SMALL, st.ver, 0), 404, FAINT, st.ver);
}

#include "mcard.h"

static void flip(void)
{
	gsKit_queue_exec(gs);
	gsKit_sync_flip(gs);
	gsKit_TexManager_nextFrame(gs);
}

static void draw(void)
{
	char l2[80];

	backdrop();
	gsKit_TexManager_bind(gs, &ftex);

	if (screen != SCR_NOCMD && screen != SCR_NOSTAGE2 && (at_home || in_mc || in_ps1))
	{
		if (at_home)
			draw_home();
		else if (in_mc)
			draw_mc();
		else
			draw_ps1();
		flip();
		frame++;
		if (note_frames > 0)
			note_frames--;
		return;
	}

	switch (screen)
	{
	case SCR_GAMES:
		draw_games();
		break;
	case SCR_SEARCH:
		draw_search();
		break;
	case SCR_NOCARD:
		if (st.card == E_NOMEDIUM)
			draw_message("No card", "There is no card in the PS3's card reader.", "Put the SD card in, then press SELECT.", "SELECT", "Look again");
		else if (st.card == E_NOFILE)
			draw_message("No games folder", "The card has no PS2ISO folder.", "Game files go in a folder named PS2ISO.", "SELECT", "Look again");
		else if (st.card == 0)
			draw_message("No games", "The card's PS2ISO folder holds no .iso files.", "", "SELECT", "Look again");
		else
		{
			snprintf(l2, sizeof l2, "It must be FAT32 or exFAT. (code %d)", st.card);
			draw_message("Card not readable", "The card reader or the card could not be read.", l2, "SELECT", "Look again");
		}
		break;
	case SCR_NOSTAGE2:
		draw_message("Not ready", "The Prism disc reader did not answer.", "On the XMB, check that webMAN ran onxmb.bat.", "PS", "Quit Game");
		break;
	default:
		draw_message("Game not found", "The SD card or this game's file is missing.", "Put the card in the PS3's reader and start again.", "PS", "Quit Game");
		break;
	}

	flip();
	frame++;
	if (note_frames > 0)
		note_frames--;
}

static void say(const char *s)
{
	strncpy(note, s, sizeof note - 1);
	note[sizeof note - 1] = 0;
	note_frames = 60 * 7;
}

/* The STARTING screen (also used for other waits, with g NULL). Drawn twice so that both frame
 * buffers hold it while the program is busy inside a call. */
static void progress(const game_t *g, const char *head, const char *title, float done, const char *what)
{
	char line[100];
	int i;

	for (i = 0; i < 2; i++)
	{
		backdrop();
		gsKit_TexManager_bind(gs, &ftex);
		top_bar("PRISM");
		if (g)
			jacket(g, 320.0f, 152.0f, 96.0f, 1.0f, 0);
		if (g)
			frame_rect(266.0f, 98.0f, 374.0f, 206.0f, 2.0f, GLOW);
		text_centre(F_SMALL, 320, 226, 5, DIM, head);
		fit(line, sizeof line, F_HEAD, title, RIGHT - LEFT);
		text_centre(F_HEAD, 320, 246, 0, INK, line);
		rect(172.0f, 292.0f, 468.0f, 296.0f, LINE);
		spectrum_bar(172.0f, 172.0f + 296.0f * done, 292.0f, 4.0f, 4, 0x80);
		text_centre(F_SMALL, 320, 308, 0, DIM, what);
		flip();
	}
}

/* ------------------------------------------------------------------ the command file */

static int cmd_read(int sector, int count)
{
	if (cmd_fd < 0)
		return -1;
	if (fioLseek(cmd_fd, sector * SECTOR, SEEK_SET) < 0)
		return -1;

	return fioRead(cmd_fd, secbuf, count * SECTOR);
}

static int num_after(const char *text0, const char *key)
{
	const char *p = strstr(text0, key);

	return p ? atoi(p + strlen(key)) : 0;
}

static void read_state(void)
{
	char *t = (char *)secbuf, *p;

	memset(&st, 0, sizeof st);
	if (cmd_read(0, 1) != SECTOR)
		return;
	secbuf[SECTOR - 1] = 0;
	if (strncmp(t, "PRISMCAT1\n", 10) != 0)
		return;

	st.ok = 1;
	st.card = num_after(t, "\ncard=");
	st.count = num_after(t, "\ncount=");
	st.more = num_after(t, "\nmore=");
	st.pin = num_after(t, "\npin=");
	st.pins = num_after(t, "\npins=");
	st.seq = num_after(t, "\nseq=");
	st.act = num_after(t, "\nact=");
	st.last = num_after(t, "\nlast=");
	st.art = num_after(t, "\nart=");
	p = strstr(t, "\nver=");
	if (p)
	{
		unsigned i;

		for (p += 5, i = 0; p[i] && p[i] != '\n' && i < sizeof st.ver - 1; i++)
			st.ver[i] = p[i];
		st.ver[i] = 0;
	}
}

static int fold(int c)
{
	return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

/* what a name is filed under: a leading "The " does not count */
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

/* What the screen shows for a game: the name without its extension (a character outside ASCII
 * shows as one '?'), the letter it is filed under, two initials and a tint for its jacket. */
static void describe(game_t *g)
{
	const unsigned char *s = (const unsigned char *)g->name;
	const char *dot = strrchr(g->name, '.'), *k, *p;
	unsigned n = 0, end = dot ? (unsigned)(dot - g->name) : strlen(g->name), i, h = 0;
	int c, from_name = !g->shown[0];

	/* a proper title from the art file wins; otherwise the file's name */
	for (i = 0; from_name && i < end && n < sizeof g->shown - 2; i++)
	{
		if (s[i] < 0x80)
			g->shown[n++] = (s[i] >= 32 && s[i] < 127) ? (char)s[i] : '?';
		else if (s[i] >= 0xC0)
			g->shown[n++] = '?';
	}
	if (n)
		g->shown[n] = 0;

	k = sort_key(g->shown);
	c = fold((unsigned char)k[0]);
	g->letter = (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 1) : 0;

	/* initials: the first letter or digit, then the one that starts the next word (or follows) */
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

/* The art file (bench/lr_art.py), if the console has one: for each title ID a proper title and
 * the slot of its cover. Covers themselves are read when they come into view (cover_load). */
static void read_art(void)
{
	int count, s, i, j;

	art_data = 0;
	if (st.art <= 0 || cmd_read(MENU_ART, 1) != SECTOR)
		return;
	secbuf[SECTOR - 1] = 0;
	if (strncmp((char *)secbuf, "PRISMART1\n", 10) != 0)
		return;
	count = num_after((char *)secbuf, "\ncount=");
	i = num_after((char *)secbuf, "\ndata=");
	if (count > CAT_MAX)
		count = CAT_MAX;
	if (i < 17 || num_after((char *)secbuf, "\nw=") != COVER || num_after((char *)secbuf, "\nh=") != COVER)
		return;

	for (s = 0; s * 16 < count; s += 8)
	{
		if (cmd_read(MENU_ART + 1 + s, 8) != 8 * SECTOR)
			return;
		for (j = 0; j < 128 && s * 16 + j < count; j++)
		{
			const unsigned char *r = secbuf + 128 * j;
			int slot = r[16] | (r[17] << 8), k;

			for (k = 0; k < n_games; k++)
			{
				if (!games[k].id[0] || strncmp(games[k].id, (const char *)r, 11) != 0)
					continue;
				if (r[20])
					strncpy(games[k].shown, (const char *)r + 20, sizeof games[k].shown - 1);
				games[k].art = (short)(slot == 0xFFFF ? -1 : slot);
			}
		}
	}
	art_data = i;
}

static void read_games(void)
{
	int total = st.count > CAT_MAX ? CAT_MAX : st.count, got = 0, s, i, j;

	n_games = 0;
	for (s = 0; got < total; s += 8)
	{
		if (cmd_read(MENU_CAT + s, 8) != 8 * SECTOR)
			break;
		for (i = 0; i < 8 * 16 && got < total; i++, got++)
		{
			const unsigned char *r = secbuf + 128 * i;
			game_t *g;

			if (!r[0])
				continue;
			g = &games[n_games++];
			memset(g, 0, sizeof *g);
			memcpy(g->name, r + 8, 103);
			memcpy(g->id, r + 112, 11);
			g->art = -1;
			g->secs = r[4] | (r[5] << 8) | (r[6] << 16) | ((unsigned)r[7] << 24);
			g->idx = (unsigned char)(s * 16 + i);
			g->on_hdd = r[1];
			g->kind = r[2];
		}
	}

	read_art();
	for (i = 0; i < n_games; i++)
		describe(&games[i]);

	/* A to Z */
	for (i = 1; i < n_games; i++)
	{
		game_t t = games[i];

		for (j = i; j > 0 && name_cmp(games[j - 1].shown, t.shown) > 0; j--)
			games[j] = games[j - 1];
		games[j] = t;
	}
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

/* Rebuild the list on screen from the search text. keep = a game number to stay on, or -1. */
static void filter(int keep)
{
	int i;

	n_view = 0;
	sel = 0;
	for (i = 0; i < n_games; i++)
	{
		if (matches(games[i].shown, query))
		{
			if (i == keep)
				sel = n_view;
			view[n_view++] = (short)i;
		}
	}
	pos = (float)sel;
}

/* Ask the stage2 where things stand and choose the screen. keep = a name to stay on. */
static void refresh(const char *keep)
{
	int i, k = -1;

	read_state();
	n_games = 0;
	query[0] = 0;
	covers_forget();
	if (cmd_fd < 0)
		screen = SCR_NOCMD;
	else if (!st.ok)
		screen = SCR_NOSTAGE2;
	else
	{
		if (st.card == 0)
			read_games();
		screen = n_games ? SCR_GAMES : SCR_NOCARD;
	}

	for (i = 0; keep && i < n_games; i++)
	{
		if (!strcmp(games[i].name, keep))
			k = i;
	}
	filter(k);
}

/* ------------------------------------------------------------------ pad, modules */

static void io_up(void)
{
	SifLoadModule("rom0:XSIO2MAN", 0, NULL);
	SifLoadModule("rom0:XPADMAN", 0, NULL);
	/* mcInit waits for ever for its server: only call it when both modules loaded */
	mc_up = SifLoadModule("rom0:XMCMAN", 0, NULL) >= 0 && SifLoadModule("rom0:XMCSERV", 0, NULL) >= 0;
	if (mc_up)
		mc_up = mcInit(MC_TYPE_XMC) >= 0;
	padInit(0);
	padPortOpen(0, 0, padbuf[0]);
	padPortOpen(1, 0, padbuf[1]);
	cmd_fd = fioOpen(CMD_FILE, FIO_O_RDONLY);
}

static void io_down(void)
{
	mc_ld_finish();
	mcReset();
	mc_up = 0;
	if (cmd_fd >= 0)
		fioClose(cmd_fd);
	cmd_fd = -1;
	padPortClose(0, 0);
	padPortClose(1, 0);
	padEnd();
}

static void iop_restart(void)
{
	SifLoadFileExit();
	fioExit();
	SifExitRpc();
	SifInitRpc(0);
	while (!SifIopReset("", 0))
		;
	while (!SifIopSync())
		;
	SifInitRpc(0);
	SifLoadFileInit();
	fioInit();
}

#ifdef DEMO
static const struct
{
	unsigned at;
	unsigned btn;
} script[] = {
#if DEMO == 2
	{ 150, PAD_RIGHT }, { 200, PAD_RIGHT }, { 260, PAD_CROSS },	/* HOME -> MEMORY CARD */
	{ 700, PAD_RIGHT }, { 760, PAD_RIGHT }, { 900, PAD_DOWN }, { 1000, PAD_LEFT },
	{ 1300, PAD_CROSS }, { 1400, PAD_UP }, { 1440, PAD_UP }, { 1500, PAD_CROSS },	/* options -> copy */
	{ 1700, PAD_RIGHT }, { 1800, PAD_CROSS },				/* yes */
	{ 2300, PAD_R1 },						/* slot 2: the copy is there */
	{ 2700, PAD_CROSS }, { 2800, PAD_UP }, { 2900, PAD_CROSS },	/* options -> delete */
	{ 3100, PAD_RIGHT }, { 3200, PAD_CROSS },				/* yes */
	{ 3700, PAD_L1 }, { 4000, PAD_CIRCLE }, { 4100, PAD_LEFT }, { 4200, PAD_CROSS },	/* back, PS1 notice */
#else
	{ 60, PAD_CROSS },				/* HOME -> PS2 */
	{ 120, PAD_RIGHT }, { 140, PAD_RIGHT }, { 160, PAD_RIGHT }, { 180, PAD_RIGHT }, { 200, PAD_RIGHT }, { 220, PAD_RIGHT },
	{ 400, PAD_R1 }, { 430, PAD_R1 },
	{ 600, PAD_TRIANGLE },				/* search: type K, A */
	{ 640, PAD_DOWN }, { 660, PAD_RIGHT }, { 680, PAD_CROSS }, { 700, PAD_UP }, { 720, PAD_LEFT }, { 740, PAD_CROSS },
	{ 1000, PAD_START }, { 1100, PAD_RIGHT },
	{ 1300, PAD_CROSS },				/* start (cannot switch discs on a PC) */
	{ 1800, PAD_TRIANGLE }, { 1830, PAD_TRIANGLE },	/* search again, then all games */
#endif
};

static unsigned buttons(void)
{
	unsigned i;

	for (i = 0; i < sizeof script / sizeof script[0]; i++)
	{
		if (frame >= script[i].at && frame < script[i].at + 4)
			return script[i].btn;
	}

	return 0;
}
#else
static unsigned buttons(void)
{
	struct padButtonStatus b;
	unsigned held = 0;
	int port;

	for (port = 0; port < 2; port++)
	{
		int s = padGetState(port, 0);

		if ((s == PAD_STATE_STABLE || s == PAD_STATE_FINDCTP1) && padRead(port, 0, &b))
			held |= 0xffff ^ b.btns;
	}

	return held;
}
#endif

/* ------------------------------------------------------------------ actions */

/* Copy the BOOT2 path ("cdrom0:\SLUS_123.45;1") out of the disc's SYSTEM.CNF. */
static int boot_path(char *out, unsigned outsz)
{
	unsigned j;
	char *p;
	int fd, n;

	fd = fioOpen(CNF_FILE, FIO_O_RDONLY);
	if (fd < 0)
		return -1;
	n = fioRead(fd, secbuf, SECTOR - 1);
	fioClose(fd);
	if (n <= 0)
		return -2;
	secbuf[n] = 0;

	p = strstr((char *)secbuf, "BOOT2");
	if (!p)
		return -3;
	p = strchr(p, '=');
	if (!p)
		return -4;
	for (p++; *p == ' ' || *p == '\t'; p++)
		;
	for (j = 0; p[j] && p[j] != '\r' && p[j] != '\n' && p[j] != ' ' && j < outsz - 1; j++)
		out[j] = p[j];
	out[j] = 0;

	return j ? 0 : -5;
}

static void wait_frames(int n)
{
	while (n-- > 0)
		gsKit_vsync_wait();
}

static void pin_game(int gi);

/* force (R2 held): for a CD or two-layer game, add its shortcut instead of the direct start.
 * (Console run C4, stage2 build m: the direct start now shows the emulator a tray open/close.) */
static void start_game(int gi, int force)
{
	const game_t *g = &games[gi];
	char path[64], why[100], keep[120], q[QUERY_MAX + 1];
	int seq0 = st.seq, r = -1, i;
	/* A CD, or a DVD too big for one layer, is not the disc the launcher title said it was. The
	 * stage2 (build k) puts such a game in as a disc CHANGE the emulator can see, which takes a
	 * few seconds; until it is through, the disc is still our own. */
	int change = g->kind != 0 || g->secs > 2298496u;

	if (change && force)
	{
		/* Console run C1: started from here, Crazy Taxi (a CD) was not readable and MGS2 Substance
		 * (two layers) stopped at its second layer: the emulator keeps the disc type and size it
		 * was started with. Started from its own shortcut on the XMB, Crazy Taxi ran. So such a
		 * game gets its shortcut here, and is started there. */
		const char *what = g->kind ? "CD" : "two-layer";

		if (g->on_hdd && g->queued != 2)
			snprintf(why, sizeof why, "A %s game starts from the XMB: it is there, under [SDCARD].", what);
		else if (st.pin)
		{
			if (!g->queued)
				pin_game(gi);
			if (g->queued != 1)
				return;		/* pin_game said why not */
			snprintf(why, sizeof why, "A %s game starts from the XMB. Added: quit, it is under [SDCARD].", what);
		}
		else
			snprintf(why, sizeof why, "A %s game starts from a shortcut on the XMB (not set up here).", what);
		say(why);
		return;
	}

	strcpy(keep, g->name);
	strcpy(q, query);
	progress(g, "STARTING", g->shown, 0.15f, "Switching the disc");

	cmd_read(MENU_ACT + MENU_STEP * g->idx, 1);
	progress(g, "STARTING", g->shown, 0.45f, "Restarting the disc modules");
	wait_frames(30);
	io_down();
	iop_restart();

	progress(g, "STARTING", g->shown, 0.75f, change ? "Changing the disc" : "Reading the game disc");
	for (i = 0; i < (change ? 16 : 10); i++)
	{
		r = boot_path(path, sizeof path);
		if (r == 0 && (!change || !strstr(path, OWN_BOOT)))
			break;
		wait_frames(change ? 45 : 30);
		if (change)
			iop_restart();	/* so that the disc modules look at the disc afresh */
	}

	if (r == 0 && !strstr(path, OWN_BOOT))
	{
		progress(g, "STARTING", g->shown, 1.0f, "Handing over");
		fioExit();
		SifExitRpc();
		FlushCache(0);
		FlushCache(2);
		LoadExecPS2(path, 0, NULL);
	}

	/* Still our own disc: say why and go back to where we were. */
	io_up();
	refresh(keep);
	if (screen == SCR_GAMES && q[0])
	{
		strcpy(query, q);
		for (i = 0; i < n_games && strcmp(games[i].name, keep); i++)
			;
		filter(i < n_games ? i : -1);
		if (!n_view)
		{
			query[0] = 0;
			filter(-1);
		}
	}
	if (!st.ok || st.seq == seq0)
		say("Not started: the disc reader did not answer.");
	else if (st.last == E_NOMEDIUM)
		say("Not started: the card is not there any more.");
	else
	{
		snprintf(why, sizeof why, "Not started: the file could not be opened (code %d).", st.last);
		say(why);
	}
}

static void pin_game(int gi)
{
	char why[100];
	int seq0 = st.seq;

	if (games[gi].queued)
	{
		say(games[gi].queued == 1 ? "Already added: it shows on the XMB after you quit." : "Already removed: it leaves the XMB after you quit.");
		return;
	}

	/* the stage2 adds a shortcut, or removes the one that is there */
	cmd_read(MENU_ACT + MENU_STEP * (ACT_PIN + games[gi].idx), 1);
	read_state();
	if (!st.ok || st.seq == seq0)
		say("Nothing changed: the disc reader did not answer.");
	else if (st.last == 0)
	{
		games[gi].queued = 1;
		say("Added. It shows on the XMB after you quit to it.");
	}
	else if (st.last == 1)
	{
		games[gi].queued = 2;
		say("Removed. It leaves the XMB after you quit to it.");
	}
	else if (st.last == E_PINNED)
		say("Not changed: the file of that name on the XMB is not a shortcut.");
	else if (st.last == E_PINNAME)
		say("Not added: rename the file on the card (plain letters, no , ; = * ?).");
	else if (st.last == E_PINFULL)
		say("Not changed: quit to the XMB once, then do more.");
	else if (st.last == E_PINKIT)
		say("Not added: the shortcut files are not on the console.");
	else
	{
		snprintf(why, sizeof why, "Not changed (code %d).", st.last);
		say(why);
	}
}

static void rescan(void)
{
	char keep[120];

	keep[0] = 0;
	if (screen == SCR_GAMES && n_view)
		strcpy(keep, games[view[sel]].name);
	progress(NULL, "ONE MOMENT", "Looking for the card", 0.5f, "This takes a few seconds.");
	cmd_read(MENU_ACT + MENU_STEP * ACT_RESCAN, 1);
	refresh(keep[0] ? keep : NULL);
}

/* L1 / R1: the first game of the letter before / after this one's */
static void jump_letter(int dir)
{
	int cur = games[view[sel]].letter, i;

	if (dir > 0)
	{
		for (i = sel + 1; i < n_view && games[view[i]].letter == cur; i++)
			;
		sel = i < n_view ? i : 0;
	}
	else
	{
		/* to the start of this letter, or from there to the start of the one before */
		for (i = sel; i > 0 && games[view[i - 1]].letter == cur; i--)
			;
		if (i == sel)
		{
			i = sel > 0 ? sel - 1 : n_view - 1;
			cur = games[view[i]].letter;
			for (; i > 0 && games[view[i - 1]].letter == cur; i--)
				;
		}
		sel = i;
	}
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
			say("No game has that in its name.");
	}
}

/* ------------------------------------------------------------------ main */

int main(void)
{
	unsigned held, last = 0, pressed;
	const unsigned moves = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT;
	int rep = 0;

	SifInitRpc(0);
	fioInit();

	gs = gsKit_init_global();
	gs->PrimAlphaEnable = GS_SETTING_ON;
	gs->PrimAAEnable = GS_SETTING_OFF;
	gs->ZBuffering = GS_SETTING_OFF;
	gs->DoubleBuffering = GS_SETTING_ON;
	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);
	gsKit_init_screen(gs);
	gsKit_set_test(gs, GS_ZTEST_OFF);
	gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
	gsKit_set_clamp(gs, GS_CMODE_CLAMP);
	gsKit_mode_switch(gs, GS_ONESHOT);
	font_init();
	covers_init();
	icons_init();

	progress(NULL, "ONE MOMENT", "Reading the card", 0.3f, "");
	io_up();
	refresh(NULL);

	for (;;)
	{
		draw();
		cover_load();
		if (in_mc)
			mc_ld_step(0);

		held = buttons();
		pressed = held & ~last;
		if (held & moves)
		{
			/* hold to keep moving */
			if (++rep > 20 && (rep & 3) == 0)
				pressed |= held & moves;
		}
		else
			rep = 0;
		last = held;

		if (screen == SCR_NOCMD || screen == SCR_NOSTAGE2)
			continue;

		if (at_home)
		{
			home_pad(pressed);
			last = buttons();
			continue;
		}
		if (in_ps1)
		{
			if (pressed & (PAD_CIRCLE | PAD_TRIANGLE | PAD_CROSS))
			{
				in_ps1 = 0;
				at_home = 1;
			}
			continue;
		}
		if (in_mc)
		{
			mc_pad(pressed);
			last = buttons();
			continue;
		}
		if (screen != SCR_SEARCH && (pressed & PAD_CIRCLE))
		{
			at_home = 1;
			continue;
		}

		if (screen == SCR_SEARCH)
		{
			search_pad(pressed);
			continue;
		}

		if ((pressed & PAD_SELECT) || (screen == SCR_NOCARD && (pressed & PAD_TRIANGLE)))
		{
			rescan();
			last = buttons();
			continue;
		}
		if (screen != SCR_GAMES)
			continue;

		if (pressed & PAD_LEFT)
			sel = sel > 0 ? sel - 1 : n_view - 1;
		if (pressed & PAD_RIGHT)
			sel = sel < n_view - 1 ? sel + 1 : 0;
		if (pressed & PAD_L1)
			jump_letter(-1);
		if (pressed & PAD_R1)
			jump_letter(1);

		if (pressed & PAD_TRIANGLE)
		{
			kx = ky = 0;
			screen = SCR_SEARCH;
		}
		else if (pressed & PAD_SQUARE)
		{
			if (st.pin)
				pin_game(view[sel]);
			else
				say("Adding to the XMB is not set up on this console yet.");
		}
		else if (pressed & PAD_CROSS)
		{
			start_game(view[sel], (held & PAD_R2) != 0);
			last = buttons();
		}
	}

	return 0;
}
