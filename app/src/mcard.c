/* MEMORY CARD screen on the PS3 side (launcher/ui/mcard.h draw_mc, ported). Every virtual card
 * on the hard disk (/dev_hdd0/savedata/vmc: the PS2 .VM2 cards and the PS1 .VM1 cards), L1 / R1
 * to change card; each save floats and turns by its own icon (PS2: the 3D model from icon.sys,
 * PS1: the 16 x 16 animated picture). X: copy to another card of the same kind, or delete, each
 * after a question that starts on "No" (vmcw.c). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include "look.h"
#include "screens.h"
#include "vmc.h"
#include "gfx.h"
#include "ui.h"
#include "savefiles.h"	/* v0.67: saves to and from USB drives and card slots */

#define MC_COLS		4
#define MC_ROWS		2

enum { MM_BROWSE, MM_OPTIONS, MM_ASK, MM_FILES, MM_CARD, MM_NAME };	/* v0.79: the card menu, the name keyboard */

static int mc_card, mc_sel, mc_top, mc_mode, mc_pick, mc_npick;
static int mc_targets[MC_CARDS], mc_ntarget;	/* the cards a save can be copied to */
/* the action asked: -1 delete; 0.. = copy to that card; v0.67: ACT_SEND - d = write it as a file to place d
 * (savefiles.h sf_dev[d]); ACT_BRING = bring file mc_fsel (sf_file[]) into this card */
#define ACT_SEND	(-100)
#define ACT_BRING	(-200)
#define IS_SEND(a)	((a) <= ACT_SEND && (a) > ACT_SEND - SF_DEVS)
static int mc_act;
static int mc_ndev;				/* the places a save can be sent to (0 on a PS1 card) */
static int mc_fsel, mc_ftop;			/* the save file chosen in the list (MM_FILES) */
static int mc_move;				/* v0.76: the answer to "Delete it from this card afterwards?" */
static int mc_ask2;				/* v0.76: that second question is up (after "Yes" to "Send this save?") */
#define MC_FROWS	7
static char mc_note[120];
static unsigned mc_note_until;
/* v0.79: the card menu and the name keyboard (their code is above mc_draw) */
static int cm_pick, kb_x, kb_y, kb_low;
static char kb_name[VMC_NAME_MAX + 1];


static void mc_say(const char *s)
{
	snprintf(mc_note, sizeof mc_note, "%s", s);
	mc_note_until = frame + 60 * 7;
}

void mc_open(void)
{
	progress(NULL, "ONE MOMENT", "Reading the memory cards", 0.5f, "/dev_hdd0/savedata/vmc");
	vmc_load_all();
	if (mc_card >= n_cards)
		mc_card = 0;
	mc_sel = mc_top = 0;
	mc_mode = MM_BROWSE;
}

void mc_step(void)
{
}

unsigned mc_nav_sig(void)	/* v0.72: screens.h */
{
	return (unsigned)(mc_card * 4096 + mc_sel * 64 + mc_mode * 16 + mc_pick) + (unsigned)mc_fsel * 65536u + (unsigned)mc_ask2 * 8u
		+ (unsigned)(kb_x * 7 + kb_y * 131 + cm_pick * 1031 + kb_low * 3) + (unsigned)strlen(kb_name) * 4099u;	/* v0.79 */
}

/* ---- icons ---- */

static float mc_tx[MC_VERTS], mc_ty[MC_VERTS], mc_tz[MC_VERTS];
static u8 mc_tl[MC_VERTS];
static unsigned short mc_ord[MC_VERTS / 3];
static float mc_key[MC_VERTS / 3];

/* launcher/ui/mcard.h icon_draw: turned about its upright axis, tipped 12 degrees toward the
 * viewer, lit from the viewer's side, sorted far to near (no depth buffer) */
static void icon_draw(const save_t *ic, float px, float py, float size, float turn, int al)
{
	const float st = 0.2079f, ct = 0.9781f;
	float s = fsin(turn), c = fcos(turn), k = ic->k * size * 0.5f, zmax = 1.0f, dim = 1.0f;
	int i, n = ic->nv / 3, gap;

	/* v0.72 (Bananafuzzbuddy, 11-25: the turning icons "clip" and "I can see the other side of the object"). Two causes,
	 * both ours, both since the RSX turns every icon live (v0.67):
	 *  - an icon that is not the chosen one was drawn 9/10 solid (alpha 0x74) to set it back a little. Each
	 *    TRIANGLE was 9/10 solid, so the far side showed through the near side. Up to v0.66 those icons were
	 *    stills, drawn solid once and then faded as one picture. Now: solid, and darker by the same amount.
	 *  - far-to-near sorting by a triangle's middle goes wrong where triangles cross or are long. The RSX has
	 *    a depth buffer; the icon now uses it (ui_depth), so the nearer surface wins at every pixel. */
	if (GFX_GPU && al < 0x80)
	{
		dim = (float)al / 128.0f;
		al = 0x80;
	}

	for (i = 0; i < ic->nv; i++)
	{
		const short *v = ic->v + i * 8;
		float x = v[0] - ic->cx, y = v[1] - ic->cy, z = v[2] - ic->cz;
		float rx = x * c + z * s, rz = z * c - x * s;
		float nx = v[3] * c + v[5] * s, nz = v[5] * c - v[3] * s, ny = (float)v[4];
		float l;

		mc_tx[i] = px + rx * k;
		mc_ty[i] = py + (y * ct + rz * st) * k;
		mc_tz[i] = rz * ct - y * st;
		if (mc_tz[i] > zmax)
			zmax = mc_tz[i];
		if (-mc_tz[i] > zmax)
			zmax = -mc_tz[i];
		l = 0.74f - (nz * ct - ny * st) * (0.30f / 4096.0f) - (ny * ct + nz * st) * (0.14f / 4096.0f) - nx * (0.08f / 4096.0f);
		if (l < 0.50f)
			l = 0.50f;
		if (l > 1.0f)
			l = 1.0f;
		mc_tl[i] = (u8)(l * dim * 128.0f);
	}
	for (i = 0; i < n; i++)
	{
		mc_ord[i] = (unsigned short)i;
		mc_key[i] = mc_tz[3 * i] + mc_tz[3 * i + 1] + mc_tz[3 * i + 2];
	}
	for (gap = n / 2; gap > 0; gap = gap == 2 ? 1 : gap * 5 / 11)
	{
		for (i = gap; i < n; i++)
		{
			unsigned short t = mc_ord[i];
			float kt = mc_key[t];
			int j = i;

			for (; j >= gap && mc_key[mc_ord[j - gap]] < kt; j -= gap)
				mc_ord[j] = mc_ord[j - gap];
			mc_ord[j] = t;
		}
	}
	ui_depth(1);
	for (i = 0; i < n; i++)
	{
		int a = 3 * mc_ord[i], b = a + 1, d = a + 2;
		const short *va = ic->v + a * 8, *vb = ic->v + b * 8, *vd = ic->v + d * 8;
		float xy[6] = { mc_tx[a], mc_ty[a], mc_tx[b], mc_ty[b], mc_tx[d], mc_ty[d] };
		float uv[6] = { (float)va[6], (float)va[7], (float)vb[6], (float)vb[7], (float)vd[6], (float)vd[7] };
		col_t cc[3] = { RGBA(mc_tl[a], mc_tl[a], mc_tl[a], al), RGBA(mc_tl[b], mc_tl[b], mc_tl[b], al), RGBA(mc_tl[d], mc_tl[d], mc_tl[d], al) };
		/* larger mc_tz = farther (the sort above draws the largest first): 0.05 .. 0.95 of the depth range */
		float dp[3] = { 0.5f + 0.45f * mc_tz[a] / zmax, 0.5f + 0.45f * mc_tz[b] / zmax, 0.5f + 0.45f * mc_tz[d] / zmax };

		tri_tex_z(&ic->tex, xy, dp, uv, cc);
	}
	ui_depth(0);
}

/* v0.27: the saves that are not chosen show a still of their icon, drawn once (by icon_draw, onto a
 * key-coloured spare canvas) and copied each frame; only the chosen one is a live, turning model.
 * v0.7-v0.26 drew every visible icon as a turning model each frame: ~357 triangles each, ~2,500 a
 * frame, and the screen ran at 8 fps on the console (105 ms a frame, owner: "laggy", 2026-11-23;
 * PC profile: ctri 54%, the per-triangle and per-row setup of tiny triangles). */
#define STILL_KEY	0x00FF00FFu
#define STILL_SIZE	80.0f
static u32 *still_canvas;

static void make_still(save_t *e, float turn)
{
	u32 *keep = fb;
	int cx, cy, r = (int)(STILL_SIZE * 1.7f), x0, y0, x1, y1, x, y, minx = CW, miny = CH, maxx = -1, maxy = -1;

	if (!still_canvas)
	{
		still_canvas = (u32 *)malloc(CW * CH * 4);
		if (!still_canvas)
			return;
		for (x = 0; x < CW * CH; x++)
			still_canvas[x] = STILL_KEY;
	}
	canvas_point(320.0f, 224.0f, &cx, &cy);
	fb = still_canvas;
	icon_draw(e, 320.0f, 224.0f, STILL_SIZE, turn, 0x80);
	fb = keep;
	x0 = cx - r < 0 ? 0 : cx - r;
	y0 = cy - r < 0 ? 0 : cy - r;
	x1 = cx + r > CW ? CW : cx + r;
	y1 = cy + r > CH ? CH : cy + r;
	for (y = y0; y < y1; y++)
		for (x = x0; x < x1; x++)
			if (still_canvas[y * CW + x] != STILL_KEY)
			{
				if (x < minx) minx = x;
				if (x > maxx) maxx = x;
				if (y < miny) miny = y;
				if (y > maxy) maxy = y;
			}
	if (maxx >= minx && maxy >= miny)
	{
		int w = maxx - minx + 1, h = maxy - miny + 1;

		e->still = (u32 *)malloc((size_t)w * h * 4);
		if (e->still)
		{
			for (y = 0; y < h; y++)
				memcpy(e->still + y * w, still_canvas + (miny + y) * CW + minx, (size_t)w * 4);
			e->sw = (short)w;
			e->sh = (short)h;
			e->sox = (short)(minx - cx);
			e->soy = (short)(miny - cy);
		}
	}
	for (y = y0; y < y1; y++)
		for (x = x0; x < x1; x++)
			still_canvas[y * CW + x] = STILL_KEY;
}

/* a PS1 save's picture: its 16 x 16 frames in turn, a little pixelated on purpose */
static void ps1_icon_draw(const save_t *s, float cx, float cy, float size, int al)
{
	int f = s->frames > 1 ? (int)((frame / 16) % (unsigned)s->frames) : 0;
	float h = size * 0.62f;

	sprite_tex(&s->tex, cx - h / 2, cy - h / 2, 0.0f, (float)(16 * f), cx + h / 2, cy + h / 2, 16.0f, (float)(16 * f + 16), RGBA(0x80, 0x80, 0x80, al));
}

/* ---- drawing ---- */

static void sheet(float x0, float y0, float x1, float y1)
{
	rect(LX0, 0.0f, LX1, 448.0f, RGBA(0x02, 0x04, 0x0C, 0x58));
	rect(x0, y0, x1, y1, RGBA(0x0D, 0x14, 0x33, 0x80));
	frame_rect(x0, y0, x1, y1, 1.0f, LINE);
	spectrum_bar(x0, x1, y0, 2.0f, 6, 0x80);
}

static void sheet_item(int y, int chosen, const char *s)
{
	if (chosen)
	{
		rect(176.0f, (float)y - 5.0f, 464.0f, (float)y + 23.0f, RGBA(0x7F, 0xE3, 0xFF, 0x1C));
		frame_rect(176.0f, (float)y - 5.0f, 464.0f, (float)y + 23.0f, 1.0f, GLOW);
	}
	text_centre(F_BODY, 320, y, 0, chosen ? INK : DIM, s);
}

static void tabs(void)
{
	int i, x = LEFT, first = mc_card > 3 ? mc_card - 3 : 0;

	for (i = first; i < n_cards && x < RIGHT - 40; i++)
	{
		char t[40];
		int w;

		snprintf(t, sizeof t, "%s%s", cards[i].ps1 ? "PS1 " : "", cards[i].name);
		if (strlen(t) > 18)
			strcpy(t + 15, "...");
		w = text_w(F_SMALL, t, 1);
		text_ex(F_SMALL, x, 74, 1.0f, 1, i == mc_card ? INK : FAINT, t);
		if (i == mc_card)
			spectrum_bar((float)x, (float)(x + w), 92.0f, 2.0f, 3, 0x80);
		x += w + 22;
	}
}

/* ---- v0.79: new cards and card names (Bananafuzzbuddy, 11-25: "allow the creation of new cards, and the renaming of
 * cards"). START opens a short menu; a name is typed on a small keyboard. vmcnew.c holds the name rules and
 * makes the files: a new PS2 card is byte for byte the PS3's own empty card (host/vmcnew_test.sh). ---- */
enum { CM_NEW2, CM_NEW1, CM_RENAME, CM_CANCEL };
static int cm_opt[4], cm_n;
static int kb_for;
static char kb_err[140];
#define KB_COLS	10
#define KB_ROWS	4
static const char kb_keys[KB_ROWS][KB_COLS + 1] = { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ0123", "456789 -_\x01" };	/* \x01: capitals / small letters */
#define PRISM_PS1_CARD	"PRISM_PS1_1"	/* launch.c PS1CARD_NAME: PS1 games started from Prism get this card BY NAME */

static char kb_char(int x, int y)
{
	char c = kb_keys[y][x];

	return kb_low && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static void cm_open(void)
{
	cm_n = 0;
	cm_opt[cm_n++] = CM_NEW2;
	cm_opt[cm_n++] = CM_NEW1;
	if (n_cards)
		cm_opt[cm_n++] = CM_RENAME;
	cm_opt[cm_n++] = CM_CANCEL;
	cm_pick = cm_n - 1;	/* starts on "Cancel" */
	mc_mode = MM_CARD;
}

static void kb_open(int what)
{
	int i, k;

	kb_for = what;
	kb_x = kb_y = kb_low = 0;
	kb_err[0] = 0;
	if (what == CM_RENAME)
		snprintf(kb_name, sizeof kb_name, "%s", cards[mc_card].name);
	else
		for (k = 1; k < 100; k++)
		{
			/* the first "Prism PS2 Card N" that no card has */
			snprintf(kb_name, sizeof kb_name, "Prism %s Card %d", what == CM_NEW1 ? "PS1" : "PS2", k);
			for (i = 0; i < n_cards; i++)
				if (!strcasecmp(cards[i].name, kb_name))
					break;
			if (i == n_cards)
				break;
		}
	mc_mode = MM_NAME;
}

/* START on the keyboard: make the card, or rename it. A refusal stays on the keyboard and says why. */
static void kb_commit(void)
{
	char why[120], ln[200], nf[80];
	size_t l = strlen(kb_name);
	int r, i;

	while (l && kb_name[l - 1] == ' ')
		kb_name[--l] = 0;
	if (kb_for == CM_RENAME)
	{
		const char *file = strrchr(cards[mc_card].path, '/');

		file = file ? file + 1 : cards[mc_card].path;
		r = vmc_rename(VMC_DIR, file, kb_name, nf, sizeof nf, why, sizeof why);
	}
	else
	{
		progress(NULL, "ONE MOMENT", "Making the new memory card", 0.5f, kb_name);
		r = vmc_new(VMC_DIR, kb_name, kb_for == CM_NEW1, ui_now_s, why, sizeof why);
	}
	if (r)
	{
		snprintf(kb_err, sizeof kb_err, "Not done: %s.", why);
		return;
	}
	mc_open();	/* the cards again, in their order */
	for (i = 0; i < n_cards; i++)
		if (!strcmp(cards[i].name, kb_name))
			mc_card = i;
	if (kb_for == CM_RENAME)
		snprintf(ln, sizeof ln, "Renamed to %s. A card in an XMB slot needs its slot given again.", kb_name);
	else
		snprintf(ln, sizeof ln, "Made the %s card %s. It is empty and ready.", kb_for == CM_NEW1 ? "PS1" : "PS2", kb_name);
	mc_say(ln);
}

static void card_sheets(void)
{
	char ln[160], t[160];
	int k, r, c;

	if (mc_mode == MM_CARD)
	{
		static const char *const words[] = { "New PS2 card", "New PS1 card", "Rename this card", "Cancel" };
		int y0 = 150, h = 52 + 36 * cm_n;

		sheet(156.0f, (float)y0, 484.0f, (float)(y0 + h));
		text_centre(F_SMALL, 320, y0 + 14, 1, DIM, "MEMORY CARDS");
		for (k = 0; k < cm_n; k++)
			sheet_item(y0 + 48 + 36 * k, cm_pick == k, words[cm_opt[k]]);
	}
	else if (mc_mode == MM_NAME)
	{
		sheet(110.0f, 92.0f, 530.0f, 396.0f);
		text_centre(F_BOLD, 320, 106, 0, INK, kb_for == CM_RENAME ? "Rename the card" : kb_for == CM_NEW1 ? "New PS1 card: its name" : "New PS2 card: its name");
		rect(150.0f, 138.0f, 490.0f, 168.0f, RGBA(0x02, 0x04, 0x0C, 0x60));
		frame_rect(150.0f, 138.0f, 490.0f, 168.0f, 1.0f, LINE);
		text(F_BODY, 162, 144, INK, kb_name);
		if ((frame / 30) & 1)
			text(F_BODY, 162 + text_w(F_BODY, kb_name, 0), 144, GLOW, "_");
		snprintf(ln, sizeof ln, "%d OF %d", (int)strlen(kb_name), VMC_NAME_MAX);
		text_centre(F_SMALL, 320, 176, 1, FAINT, ln);
		for (r = 0; r < KB_ROWS; r++)
			for (c = 0; c < KB_COLS; c++)
			{
				float cx = 140.0f + c * 40.0f, cy = 200.0f + r * 32.0f;
				char key[6] = { kb_char(c, r), 0, 0, 0, 0, 0 };
				int on = c == kb_x && r == kb_y;

				if (key[0] == 1)
					strcpy(key, kb_low ? "ABC" : "abc");
				else if (key[0] == ' ')
					strcpy(key, "SPC");
				if (on)
				{
					rect(cx - 18.0f, cy - 4.0f, cx + 18.0f, cy + 22.0f, RGBA(0x7F, 0xE3, 0xFF, 0x1C));
					frame_rect(cx - 18.0f, cy - 4.0f, cx + 18.0f, cy + 22.0f, 1.0f, GLOW);
				}
				text_centre(key[1] ? F_SMALL : F_BODY, (int)cx, (int)cy + (key[1] ? 3 : 0), 0, on ? INK : DIM, key);
			}
		text_centre(F_SMALL, 320, 334, 0, DIM, "X types  " DOT "  Square deletes  " DOT "  Triangle: capitals or small");
		text_centre(F_SMALL, 320, 352, 0, DIM, "START: done  " DOT "  Circle: back, nothing changes");
		if (kb_err[0])
			fit(t, sizeof t, F_SMALL, kb_err, 400);
		else if (kb_for == CM_RENAME)
			fit(t, sizeof t, F_SMALL, "Its saves stay as they are. A card in an XMB slot needs its slot given again.", 400);
		else
			fit(t, sizeof t, F_SMALL, "An empty, formatted card, as the XMB makes them.", 400);
		text_centre(F_SMALL, 320, 372, 0, kb_err[0] ? AMBER : FAINT, t);
	}
}

void mc_draw(void)
{
	char ln[120], t[90];
	card_t *cd = n_cards ? &cards[mc_card] : NULL;
	save_t *s = cd && cd->n ? &cd->saves[mc_sel] : NULL;
	int r, c, x;

	top_bar("MEMORY CARD");
	if (!cd)
	{
		text_centre(F_HEAD, 320, 176, 0, INK, "No memory cards");
		text_centre(F_BODY, 320, 222, 0, DIM, "The console has no virtual memory cards yet.");
		text_centre(F_BODY, 320, 248, 0, DIM, "START makes a new one.");	/* v0.79 */
		legend_line();
		c = legend_item(LEFT, -1, "START", "New card");
		legend_icon(c, 3, "Menu");
		card_sheets();
		return;
	}
	tabs();
	if (cd->ok)
	{
		snprintf(ln, sizeof ln, "%d SAVE%s", cd->n, cd->n == 1 ? "" : "S");
		snprintf(t, sizeof t, "  " DOT "  %d KB FREE", cd->free_kb);
		top_right(ln, DIM, t, FAINT);
	}

	if (!cd->ok)
	{
		text_centre(F_HEAD, 320, 176, 0, INK, "Card not readable");
		text_centre(F_BODY, 320, 222, 0, DIM, "This card file is not a formatted memory card.");
	}
	else if (!s)
		text_centre(F_HEAD, 320, 190, 0, INK, "No saves on this card");
	else
	{
		for (r = 0; r < MC_ROWS; r++)
		{
			for (c = 0; c < MC_COLS; c++)
			{
				int i = (mc_top + r) * MC_COLS + c, chosen = i == mc_sel;
				float cx = 104.0f + c * 144.0f, cy = 150.0f + r * 104.0f;
				float bob = 5.0f * fsin(frame / 260.0f + i * 0.19f), size = chosen ? 112.0f : 80.0f;
				float turn = (chosen ? frame / 300.0f : frame / 900.0f) + i * 0.13f;
				save_t *e;

				if (i >= cd->n)
					continue;
				e = &cd->saves[i];
				if (chosen)
					quad(cx - 58.0f, cy + 46.0f, cx + 58.0f, cy + 46.0f, cx - 34.0f, cy + 54.0f, cx + 34.0f, cy + 54.0f,
						RGBA(0x7F, 0xE3, 0xFF, 0x30), RGBA(0x7F, 0xE3, 0xFF, 0x30), RGBA(0x7F, 0xE3, 0xFF, 0x00), RGBA(0x7F, 0xE3, 0xFF, 0x00));
				if (e->icon && cd->ps1)
					ps1_icon_draw(e, cx, cy + bob, size, chosen ? 0x80 : 0x74);
#ifdef MC_LIVE_ICONS	/* host/rsx_host.sh: the CPU drawing with live icons too, to compare like with like */
				else if (0)
#else
				else if (e->icon && !chosen && !GFX_GPU)
#endif
				{
					/* v0.67: only while the CPU draws; the RSX turns every icon live (the branch below) */
					/* v0.27: a still, drawn once at the angle the icon had at frame 0 */
					if (!e->still)
						make_still(e, i * 0.13f);
					if (e->still)
						blit_keyed(e->still, e->sw, e->sh, STILL_KEY, cx, cy + bob, e->sox, e->soy, 0x74);
					else
						icon_draw(e, cx, cy + bob, size, turn, 0x74);
				}
				else if (e->icon)
					icon_draw(e, cx, cy + bob, size, turn, chosen ? 0x80 : 0x74);
				else
				{
					float w = size * 0.30f * (0.35f + 0.65f * (fcos(turn * 3.0f) < 0.0f ? -fcos(turn * 3.0f) : fcos(turn * 3.0f)));

					frame_rect(cx - w, cy + bob - size * 0.30f, cx + w, cy + bob + size * 0.30f, 2.0f, chosen ? GLOW : FAINT);
				}
			}
		}
		fit(ln, sizeof ln, F_BOLD, s->title, RIGHT - LEFT);
		text_centre(F_BOLD, 320, 318, 0, INK, ln);
		if (cd->ps1)
			snprintf(ln, sizeof ln, "%s  " DOT "  %d BLOCK%s", s->dir, s->blocks, s->blocks == 1 ? "" : "S");
		else if (s->when[0])
			snprintf(ln, sizeof ln, "%s  " DOT "  %u KB  " DOT "  %s", s->dir, (s->bytes + 1023) / 1024, s->when);
		else
			snprintf(ln, sizeof ln, "%s  " DOT "  %u KB", s->dir, (s->bytes + 1023) / 1024);
		fit(t, sizeof t, F_SMALL, ln, RIGHT - LEFT);
		text_centre(F_SMALL, 320, 346, 1, DIM, t);
		snprintf(ln, sizeof ln, "%d OF %d", mc_sel + 1, cd->n);
		text_centre(F_SMALL, 320, 364, 1, FAINT, ln);
	}
	if (mc_note_until > frame)
		text_centre(F_SMALL, 320, 380, 0, AMBER, mc_note);

	legend_line();
	x = LEFT;
	if (s)
		x = legend_icon(x, 0, "Options");
	if (cd->ok && !cd->ps1)
		x = legend_icon(x, 1, "Bring in");	/* v0.67: a save file from a USB drive or a card */
	x = legend_item(x, -1, "L1 R1", "Card");
	x = legend_item(x, -1, "START", "Cards");	/* v0.79 */
	x = legend_item(x, -1, "SELECT", "Reread");
	legend_icon(x, 3, "Menu");

	if (mc_mode == MM_OPTIONS && s)
	{
		int k, y0 = 138, h = 52 + 36 * mc_npick;

		if (y0 + h > 436)
			y0 = 436 - h;	/* v0.67: more rows with the places to send to */
		sheet(156.0f, (float)y0, 484.0f, (float)(y0 + h));
		fit(ln, sizeof ln, F_SMALL, s->title, 300);
		text_centre(F_SMALL, 320, y0 + 14, 1, DIM, ln);
		for (k = 0; k < mc_npick; k++)
		{
			if (k < mc_ntarget)
				snprintf(ln, sizeof ln, "Copy to %s", cards[mc_targets[k]].name);
			else if (k < mc_ntarget + mc_ndev)
				snprintf(ln, sizeof ln, "Send to %s", sf_dev[k - mc_ntarget].name);
			else if (k == mc_ntarget + mc_ndev)
				strcpy(ln, "Delete");
			else
				strcpy(ln, "Cancel");
			fit(t, sizeof t, F_BODY, ln, 270);
			sheet_item(y0 + 48 + 36 * k, mc_pick == k, t);
		}
	}
	else if (mc_mode == MM_FILES)
	{
		/* v0.67: the save files found (savefiles.h), to bring one into this card */
		int k;

		sheet(96.0f, 92.0f, 544.0f, 372.0f);
		text_centre(F_BOLD, 320, 106, 0, INK, "Bring in a save");
		snprintf(ln, sizeof ln, "Into the card %s", cd->name);
		fit(t, sizeof t, F_SMALL, ln, 420);
		text_centre(F_SMALL, 320, 136, 0, DIM, t);
		for (k = 0; k < MC_FROWS && mc_ftop + k < n_sf_file; k++)
		{
			const sf_file_t *f = &sf_file[mc_ftop + k];
			int y = 166 + 26 * k, chosen = mc_ftop + k == mc_fsel;

			if (chosen)
			{
				rect(112.0f, (float)y - 4.0f, 528.0f, (float)y + 20.0f, RGBA(0x7F, 0xE3, 0xFF, 0x1C));
				frame_rect(112.0f, (float)y - 4.0f, 528.0f, (float)y + 20.0f, 1.0f, GLOW);
			}
			fit(t, sizeof t, F_SMALL, f->save, 250);
			text(F_SMALL, 122, y, chosen ? INK : DIM, t);
			snprintf(ln, sizeof ln, "%s  " DOT "  %u KB", f->dev, f->kb);
			text(F_SMALL, 518 - text_w(F_SMALL, ln, 0), y, chosen ? DIM : FAINT, ln);
		}
		snprintf(ln, sizeof ln, "%d OF %d", mc_fsel + 1, n_sf_file);
		text_centre(F_SMALL, 320, 350, 1, FAINT, ln);
	}
	else if (mc_mode == MM_ASK && (s || mc_act == ACT_BRING))
	{
		sheet(150.0f, 138.0f, 490.0f, 304.0f);
		if (mc_act == ACT_BRING)
		{
			/* v0.67 */
			const sf_file_t *f = &sf_file[mc_fsel];

			text_centre(F_BOLD, 320, 154, 0, INK, "Bring this save in?");
			fit(ln, sizeof ln, F_BODY, f->save, 310);
			text_centre(F_BODY, 320, 188, 0, DIM, ln);
			snprintf(ln, sizeof ln, "From %s, into %s.", f->dev, cd->name);
			fit(t, sizeof t, F_SMALL, ln, 320);
			text_centre(F_SMALL, 320, 216, 0, DIM, t);
		}
		else
		{
		text_centre(F_BOLD, 320, 154, 0, INK, mc_act >= 0 ? "Copy this save?" : mc_ask2 ? "Delete it from this card afterwards?"
			: IS_SEND(mc_act) ? "Send this save?" : "Delete this save?");
		fit(ln, sizeof ln, F_BODY, s->title, 310);
		text_centre(F_BODY, 320, 188, 0, DIM, ln);
		if (mc_ask2)
		{
			/* v0.76 (Bananafuzzbuddy, 11-25: "after the confirmation I want it to ask if i want to delete the original
			 * to which i can say yes or no") */
			snprintf(ln, sizeof ln, "The file on %s is read back first.", sf_dev[ACT_SEND - mc_act].name);
			text_centre(F_SMALL, 320, 226, 0, mc_pick ? AMBER : FAINT, mc_pick ? "Yes: the save is then deleted from this card for good." : "No: the save stays on the card too.");
		}
		else if (mc_act >= 0)
			snprintf(ln, sizeof ln, "It will be copied to %s.", cards[mc_act].name);
		else if (IS_SEND(mc_act))
		{
			/* v0.67: a second, smaller line says what happens to a file already there */
			snprintf(ln, sizeof ln, "To %s, in its folder " SF_DIR ".", sf_dev[ACT_SEND - mc_act].name);
			text_centre(F_SMALL, 320, 226, 0, sf_exists(ACT_SEND - mc_act, s->dir) ? AMBER : FAINT,
				sf_exists(ACT_SEND - mc_act, s->dir) ? "The file already there for this save is replaced." : "Next: whether it also leaves this card.");
		}
		else
			snprintf(ln, sizeof ln, "It will be gone from %s for good.", cd->name);
		fit(t, sizeof t, F_SMALL, ln, 320);
		text_centre(F_SMALL, 320, IS_SEND(mc_act) ? 208 : 216, 0, mc_act >= 0 || IS_SEND(mc_act) ? DIM : AMBER, t);
		}
		for (c = 0; c < 2; c++)
		{
			float bx = c ? 330.0f : 200.0f;

			if (mc_pick == c)
			{
				rect(bx, 250.0f, bx + 110.0f, 280.0f, RGBA(0x7F, 0xE3, 0xFF, 0x1C));
				frame_rect(bx, 250.0f, bx + 110.0f, 280.0f, 1.0f, GLOW);
			}
			text_centre(F_BODY, (int)bx + 55, 256, 0, mc_pick == c ? INK : DIM, c ? "Yes" : "No");
		}
	}
	card_sheets();	/* v0.79 */
}

/* ---- pad ---- */

static void do_action(void)
{
	char why[160], ln[200], want[33];
	card_t *cd = &cards[mc_card];
	int r, keep = mc_sel;

	want[0] = 0;
	if (mc_act == ACT_BRING)
	{
		/* v0.67: a save file into this card (vmcw.c keeps the card as it was first, as for every change) */
		const sf_file_t *f = &sf_file[mc_fsel];

		progress(NULL, "ONE MOMENT", "Bringing the save in", 0.5f, f->dev);
		r = vmc_import_psu(f->path, cd, why, sizeof why);
		if (r == 0)
		{
			snprintf(ln, sizeof ln, "Brought in from %s.", f->dev);
			snprintf(want, sizeof want, "%s", f->save);
		}
		else
			snprintf(ln, sizeof ln, "Not brought in: %s.", why);
	}
	else if (IS_SEND(mc_act))
	{
		/* v0.67: the save as a file on a USB drive or a card; the card itself is not changed */
		char shown[80];
		int d = ACT_SEND - mc_act;

		progress(NULL, "ONE MOMENT", "Writing the save file", 0.5f, sf_dev[d].name);
		r = sf_export(cd, mc_sel, d, shown, sizeof shown, why, sizeof why);
		if (r == 0)
			snprintf(ln, sizeof ln, "Written to %s: %s", sf_dev[d].name, shown);
		else
			snprintf(ln, sizeof ln, "Not written: %s.", why);
		if (r != 0 || !mc_move)
		{
			mc_move = 0;
			mc_say(ln);
			return;		/* nothing on the cards changed: no need to read them again */
		}
		/* v0.75: the file is written and was read back (sf_export): the save may leave the card */
		mc_move = 0;
		progress(NULL, "ONE MOMENT", "Deleting the save from the card", 0.8f, cd->name);
		r = vmc_delete(cd, mc_sel, why, sizeof why);
		if (r == 0)
			snprintf(ln, sizeof ln, "Moved to %s: %s", sf_dev[d].name, shown);
		else
			snprintf(ln, sizeof ln, "Written to %s, but not deleted from the card: %s.", sf_dev[d].name, why);
		/* falls through to the reload below: the card changed */
	}
	else if (mc_act >= 0)
	{
		int to = mc_act;

		progress(NULL, "ONE MOMENT", "Copying the save", 0.5f, cards[to].name);
		r = vmc_copy(cd, mc_sel, &cards[to], why, sizeof why);
		if (r == 0)
			snprintf(ln, sizeof ln, "Copied to %s.", cards[to].name);
		else
			snprintf(ln, sizeof ln, "Not copied: %s.", why);
	}
	else
	{
		progress(NULL, "ONE MOMENT", "Deleting the save", 0.5f, cd->name);
		r = vmc_delete(cd, mc_sel, why, sizeof why);
		if (r == 0)
			strcpy(ln, "Deleted.");
		else
			snprintf(ln, sizeof ln, "Not deleted: %s.", why);
	}
	/* read every card again from the hard disk: the screen shows what is really there */
	{
		int at = mc_card;

		vmc_load_all();
		mc_card = at < n_cards ? at : 0;
		mc_sel = keep < cards[mc_card].n ? keep : (cards[mc_card].n ? cards[mc_card].n - 1 : 0);
		if (want[0])
		{
			/* v0.67: the save just brought in is the one chosen */
			int k;

			for (k = 0; k < cards[mc_card].n; k++)
				if (!strcmp(cards[mc_card].saves[k].dir, want))
					mc_sel = k;
		}
		mc_top = mc_sel / MC_COLS > MC_ROWS - 1 ? mc_sel / MC_COLS - MC_ROWS + 1 : 0;
	}
	mc_say(ln);
}

int mc_pad(unsigned pressed)
{
	card_t *cd = n_cards ? &cards[mc_card] : NULL;
	int n = cd ? cd->n : 0, i;

	if (mc_mode == MM_CARD)
	{
		/* v0.79 */
		if (pressed & PAD_UP)
			cm_pick = cm_pick ? cm_pick - 1 : cm_n - 1;
		if (pressed & PAD_DOWN)
			cm_pick = cm_pick < cm_n - 1 ? cm_pick + 1 : 0;
		if (pressed & (PAD_CIRCLE | PAD_TRIANGLE | PAD_START))
			mc_mode = MM_BROWSE;
		else if (pressed & PAD_CROSS)
		{
			int o = cm_opt[cm_pick];

			mc_mode = MM_BROWSE;
			if (o == CM_RENAME && cd && !strcasecmp(cd->name, PRISM_PS1_CARD))
				mc_say("PS1 games started from Prism save to this card by this name: it keeps the name.");
			else if ((o == CM_NEW2 || o == CM_NEW1) && n_cards >= MC_CARDS)
				mc_say("Prism shows eight cards at most: there is no room for another.");
			else if (o != CM_CANCEL)
				kb_open(o);
		}
		return 0;
	}
	if (mc_mode == MM_NAME)
	{
		size_t l = strlen(kb_name);

		if (pressed & PAD_LEFT)
			kb_x = kb_x ? kb_x - 1 : KB_COLS - 1;
		if (pressed & PAD_RIGHT)
			kb_x = kb_x < KB_COLS - 1 ? kb_x + 1 : 0;
		if (pressed & PAD_UP)
			kb_y = kb_y ? kb_y - 1 : KB_ROWS - 1;
		if (pressed & PAD_DOWN)
			kb_y = kb_y < KB_ROWS - 1 ? kb_y + 1 : 0;
		if (pressed & PAD_TRIANGLE)
			kb_low ^= 1;
		if ((pressed & PAD_SQUARE) && l)
		{
			kb_name[--l] = 0;
			kb_err[0] = 0;
		}
		if (pressed & PAD_CROSS)
		{
			char ch = kb_char(kb_x, kb_y);

			kb_err[0] = 0;
			if (ch == 1)
				kb_low ^= 1;
			else if (l >= VMC_NAME_MAX)
				snprintf(kb_err, sizeof kb_err, "A card's name has %d characters at most.", VMC_NAME_MAX);
			else if (!(ch == ' ' && l == 0))
			{
				kb_name[l] = ch;
				kb_name[l + 1] = 0;
			}
		}
		if (pressed & PAD_CIRCLE)
			mc_mode = MM_BROWSE;
		else if (pressed & PAD_START)
			kb_commit();
		return 0;
	}
	if (mc_mode == MM_OPTIONS)
	{
		if (pressed & PAD_UP)
			mc_pick = mc_pick ? mc_pick - 1 : mc_npick - 1;
		if (pressed & PAD_DOWN)
			mc_pick = mc_pick < mc_npick - 1 ? mc_pick + 1 : 0;
		if (pressed & (PAD_CIRCLE | PAD_TRIANGLE))
			mc_mode = MM_BROWSE;
		else if (pressed & PAD_CROSS)
		{
			if (mc_pick < mc_ntarget)
				mc_act = mc_targets[mc_pick];
			else if (mc_pick < mc_ntarget + mc_ndev)
				mc_act = ACT_SEND - (mc_pick - mc_ntarget);
			else if (mc_pick == mc_ntarget + mc_ndev)
				mc_act = -1;
			else
			{
				mc_mode = MM_BROWSE;
				return 0;
			}
			mc_mode = MM_ASK;
			mc_pick = 0;		/* the question starts on "No" */
			mc_move = mc_ask2 = 0;	/* v0.76 */
		}
		return 0;
	}
	if (mc_mode == MM_ASK)
	{
		if (pressed & (PAD_LEFT | PAD_RIGHT))
			mc_pick ^= 1;
		if (pressed & (PAD_CIRCLE | PAD_TRIANGLE))
		{
			mc_mode = MM_BROWSE;	/* at either question: nothing is sent */
			mc_move = mc_ask2 = 0;
		}
		else if (pressed & PAD_CROSS)
		{
			if (mc_ask2)
			{
				/* v0.76: the second answer; either way the save is sent now */
				mc_move = mc_pick;
				mc_ask2 = 0;
				mc_mode = MM_BROWSE;
				if (n)
					do_action();
			}
			else if (mc_pick == 1 && n && IS_SEND(mc_act))
			{
				mc_ask2 = 1;		/* v0.76: "Yes, send it": now, should it leave the card? */
				mc_pick = 0;		/* starts on "No" */
			}
			else
			{
				mc_mode = MM_BROWSE;
				if (mc_pick == 1 && (n || mc_act == ACT_BRING))
					do_action();
			}
		}
		return 0;
	}
	if (mc_mode == MM_FILES)
	{
		/* v0.67 */
		if ((pressed & PAD_UP) && mc_fsel > 0)
			mc_fsel--;
		if ((pressed & PAD_DOWN) && mc_fsel < n_sf_file - 1)
			mc_fsel++;
		if (mc_fsel < mc_ftop)
			mc_ftop = mc_fsel;
		if (mc_fsel >= mc_ftop + MC_FROWS)
			mc_ftop = mc_fsel - MC_FROWS + 1;
		if (pressed & (PAD_CIRCLE | PAD_TRIANGLE | PAD_SQUARE))
			mc_mode = MM_BROWSE;
		else if ((pressed & PAD_CROSS) && n_sf_file)
		{
			mc_act = ACT_BRING;
			mc_mode = MM_ASK;
			mc_pick = 0;		/* the question starts on "No" */
			mc_move = mc_ask2 = 0;	/* v0.76 */
		}
		return 0;
	}
	if (pressed & (PAD_CIRCLE | PAD_TRIANGLE))
		return 1;
	if (pressed & PAD_START)
	{
		cm_open();	/* v0.79: also with no card at all */
		return 0;
	}
	if (!cd)
		return 0;
	if (pressed & (PAD_L1 | PAD_R1))
	{
		mc_card = (pressed & PAD_R1) ? (mc_card + 1) % n_cards : (mc_card + n_cards - 1) % n_cards;
		mc_sel = mc_top = 0;
		return 0;
	}
	if (pressed & PAD_SELECT)
	{
		mc_open();
		return 0;
	}
	if (pressed & PAD_SQUARE)
	{
		/* v0.67: bring a save file in. Only PS2 saves travel as .psu files */
		if (!cd->ok || cd->ps1)
			mc_say("Save files (.psu) hold PS2 saves: choose a PS2 card with L1 or R1 first.");
		else
		{
			progress(NULL, "ONE MOMENT", "Looking for save files", 0.5f, "The folder " SF_DIR " on USB drives and cards");
			sf_scan_devs();
			sf_scan_files();
			if (!n_sf_file)
				mc_say("No save files found. They go in a folder " SF_DIR " on a USB drive or a card, as .psu files.");
			else
			{
				mc_fsel = mc_ftop = 0;
				mc_mode = MM_FILES;
			}
		}
		return 0;
	}
	if (!n)
		return 0;
	if ((pressed & PAD_LEFT) && mc_sel > 0)
		mc_sel--;
	if ((pressed & PAD_RIGHT) && mc_sel < n - 1)
		mc_sel++;
	if ((pressed & PAD_UP) && mc_sel >= MC_COLS)
		mc_sel -= MC_COLS;
	if ((pressed & PAD_DOWN) && mc_sel + MC_COLS < n)
		mc_sel += MC_COLS;
	else if ((pressed & PAD_DOWN) && mc_sel / MC_COLS < (n - 1) / MC_COLS)
		mc_sel = n - 1;
	if (mc_sel / MC_COLS < mc_top)
		mc_top = mc_sel / MC_COLS;
	if (mc_sel / MC_COLS >= mc_top + MC_ROWS)
		mc_top = mc_sel / MC_COLS - MC_ROWS + 1;
	if (pressed & PAD_CROSS)
	{
		mc_ntarget = 0;
		for (i = 0; i < n_cards; i++)
			if (i != mc_card && cards[i].ok && cards[i].ps1 == cd->ps1)
				mc_targets[mc_ntarget++] = i;
		/* v0.67: the places it can be sent to as a file (PS2 saves only) */
		mc_ndev = 0;
		if (!cd->ps1)
		{
			sf_scan_devs();
			mc_ndev = n_sf_dev;
		}
		mc_npick = mc_ntarget + mc_ndev + 2;
		mc_mode = MM_OPTIONS;
		mc_pick = mc_npick - 1;	/* starts on "Cancel" */
	}
	return 0;
}
