/* MEMORY CARD screen on the PS3 side (launcher/ui/mcard.h draw_mc, ported). Every virtual card
 * on the hard disk (/dev_hdd0/savedata/vmc: the PS2 .VM2 cards and the PS1 .VM1 cards), L1 / R1
 * to change card; each save floats and turns by its own icon (PS2: the 3D model from icon.sys,
 * PS1: the 16 x 16 animated picture). X: copy to another card of the same kind, or delete, each
 * after a question that starts on "No" (vmcw.c). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "look.h"
#include "screens.h"
#include "vmc.h"
#include "gfx.h"
#include "ui.h"

#define MC_COLS		4
#define MC_ROWS		2

enum { MM_BROWSE, MM_OPTIONS, MM_ASK };

static int mc_card, mc_sel, mc_top, mc_mode, mc_pick, mc_npick;
static int mc_targets[MC_CARDS], mc_ntarget;	/* the cards a save can be copied to */
static int mc_act;				/* the action asked: -1 delete, else target card */
static char mc_note[120];
static unsigned mc_note_until;


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
	float s = fsin(turn), c = fcos(turn), k = ic->k * size * 0.5f;
	int i, n = ic->nv / 3, gap;

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
		l = 0.74f - (nz * ct - ny * st) * (0.30f / 4096.0f) - (ny * ct + nz * st) * (0.14f / 4096.0f) - nx * (0.08f / 4096.0f);
		if (l < 0.50f)
			l = 0.50f;
		if (l > 1.0f)
			l = 1.0f;
		mc_tl[i] = (u8)(l * 128.0f);
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
	for (i = 0; i < n; i++)
	{
		int a = 3 * mc_ord[i], b = a + 1, d = a + 2;
		const short *va = ic->v + a * 8, *vb = ic->v + b * 8, *vd = ic->v + d * 8;
		float xy[6] = { mc_tx[a], mc_ty[a], mc_tx[b], mc_ty[b], mc_tx[d], mc_ty[d] };
		float uv[6] = { (float)va[6], (float)va[7], (float)vb[6], (float)vb[7], (float)vd[6], (float)vd[7] };
		col_t cc[3] = { RGBA(mc_tl[a], mc_tl[a], mc_tl[a], al), RGBA(mc_tl[b], mc_tl[b], mc_tl[b], al), RGBA(mc_tl[d], mc_tl[d], mc_tl[d], al) };

		tri_tex(&ic->tex, xy, uv, cc);
	}
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
		text_centre(F_BODY, 320, 248, 0, DIM, "Make one on the XMB, under Memory Card Utility.");
		legend_line();
		legend_icon(LEFT, 3, "Menu");
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
				else if (e->icon && !chosen)
				{
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
		x = legend_icon(x, 0, "Copy or delete");
	x = legend_item(x, -1, "L1 R1", "Card");
	x = legend_item(x, -1, "SELECT", "Read again");
	legend_icon(x, 3, "Menu");

	if (mc_mode == MM_OPTIONS && s)
	{
		int k, y0 = 138, h = 52 + 36 * mc_npick;

		sheet(156.0f, (float)y0, 484.0f, (float)(y0 + h));
		fit(ln, sizeof ln, F_SMALL, s->title, 300);
		text_centre(F_SMALL, 320, y0 + 14, 1, DIM, ln);
		for (k = 0; k < mc_npick; k++)
		{
			if (k < mc_ntarget)
				snprintf(ln, sizeof ln, "Copy to %s", cards[mc_targets[k]].name);
			else if (k == mc_ntarget)
				strcpy(ln, "Delete");
			else
				strcpy(ln, "Cancel");
			fit(t, sizeof t, F_BODY, ln, 270);
			sheet_item(y0 + 48 + 36 * k, mc_pick == k, t);
		}
	}
	else if (mc_mode == MM_ASK && s)
	{
		sheet(150.0f, 138.0f, 490.0f, 304.0f);
		text_centre(F_BOLD, 320, 154, 0, INK, mc_act >= 0 ? "Copy this save?" : "Delete this save?");
		fit(ln, sizeof ln, F_BODY, s->title, 310);
		text_centre(F_BODY, 320, 188, 0, DIM, ln);
		if (mc_act >= 0)
			snprintf(ln, sizeof ln, "It will be copied to %s.", cards[mc_act].name);
		else
			snprintf(ln, sizeof ln, "It will be gone from %s for good.", cd->name);
		fit(t, sizeof t, F_SMALL, ln, 320);
		text_centre(F_SMALL, 320, 216, 0, mc_act >= 0 ? DIM : AMBER, t);
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
}

/* ---- pad ---- */

static void do_action(void)
{
	char why[160], ln[200];
	card_t *cd = &cards[mc_card];
	int r, keep = mc_sel;

	if (mc_act >= 0)
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
		mc_top = mc_sel / MC_COLS > MC_ROWS - 1 ? mc_sel / MC_COLS - MC_ROWS + 1 : 0;
	}
	mc_say(ln);
}

int mc_pad(unsigned pressed)
{
	card_t *cd = n_cards ? &cards[mc_card] : NULL;
	int n = cd ? cd->n : 0, i;

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
			else if (mc_pick == mc_ntarget)
				mc_act = -1;
			else
			{
				mc_mode = MM_BROWSE;
				return 0;
			}
			mc_mode = MM_ASK;
			mc_pick = 0;		/* the question starts on "No" */
		}
		return 0;
	}
	if (mc_mode == MM_ASK)
	{
		if (pressed & (PAD_LEFT | PAD_RIGHT))
			mc_pick ^= 1;
		if (pressed & (PAD_CIRCLE | PAD_TRIANGLE))
			mc_mode = MM_BROWSE;
		else if (pressed & PAD_CROSS)
		{
			mc_mode = MM_BROWSE;
			if (mc_pick == 1 && n)
				do_action();
		}
		return 0;
	}
	if (pressed & (PAD_CIRCLE | PAD_TRIANGLE))
		return 1;
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
		mc_npick = mc_ntarget + 2;
		mc_mode = MM_OPTIONS;
		mc_pick = mc_npick - 1;	/* starts on "Cancel" */
	}
	return 0;
}
