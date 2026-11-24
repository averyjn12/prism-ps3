/* mcard.h - the HOME menu and the MEMORY CARD screen of the Prism launcher (2026-11-22).
 * Included by main.c (one translation unit; everything here is static).
 *
 * HOME         PS2 / PS1 / MEMORY CARD. PS1 is a notice: PS1 games run in another emulator
 *              that only the XMB can start.
 * MEMORY CARD  the saves of the PS2 memory cards in slot 1 and slot 2 (under the PS3 these are
 *              the two virtual cards chosen on the XMB), each shown by its own 3D icon, turning
 *              and drifting; a save can be copied to the other card or deleted, each after a
 *              question whose answer starts on "No".
 *
 * Cards are reached through the console's own modules (rom0:XMCMAN / XMCSERV, libmc).
 *
 * A save's picture: its icon.sys names an icon file; the file holds a list of triangles (one
 * or more shapes for animation - the first is used), an animation table, and a 128 x 128
 * 16-bit texture, plain or run-length packed. The reading was checked with icon_check.py
 * against the nine real icons on the owner's cards (every file consumed to its last byte).
 * No depth buffer: the triangles are sorted far to near on every frame.
 */
#include <libmc.h>

#define MC_SAVES	48
#define MC_FILES	32
#define MC_ICONS	12
#define MC_VERTS	3000
#define MC_COLS		4
#define MC_ROWS		2
#define ICON_PX		128

typedef struct
{
	char dir[33];
	char title[72];		/* from icon.sys, in the font's characters */
	char iconfile[33];
	unsigned bytes;		/* all its files */
	unsigned iconsize;
	int nfiles;
	sceMcStDateTime when;
	signed char icon;	/* slot in mc_icons; -1 not read yet; -2 cannot be shown */
} save_t;

typedef struct
{
	GSTEXTURE tex;
	short *v;		/* nv x (x, y, z, nx, ny, nz, u, v), u v in texels */
	int nv;
	float cx, cy, cz, k;	/* middle of the model, and 1 / its half size */
	int port, save;		/* whose it is; port -1 = free */
	unsigned used;
} mcicon_t;

static void progress(const game_t *g, const char *head, const char *title, float done, const char *what);
static void say(const char *s);

static save_t mc_saves[2][MC_SAVES];
static int mc_n[2], mc_card[2], mc_free[2];	/* card: 1 a PS2 card, 0 none, -2 not formatted */
static int mc_port, mc_sel, mc_top, mc_mode, mc_pick;
static int mc_up, mc_seen;
static int home_sel, at_home = 1, in_mc, in_ps1;
static sceMcTblGetDir mc_tab[MC_SAVES + 2] __attribute__((aligned(64)));
static sceMcTblGetDir mc_ftab[MC_FILES + 2] __attribute__((aligned(64)));
static sceMcTblGetDir mc_dinfo __attribute__((aligned(64)));
static unsigned char mc_sys[1024] __attribute__((aligned(64)));
static mcicon_t mc_icons[MC_ICONS];

enum { MM_BROWSE, MM_OPTIONS, MM_ASK_COPY, MM_ASK_DELETE };

static int mc_wait(void)
{
	int cmd = 0, res = -1;

	mcSync(MC_WAIT, &cmd, &res);

	return res;
}

/* ---- names ---- */

/* icon.sys titles are Shift-JIS, nearly always full-width Latin. Into the font's characters;
 * brk = where the second line starts. */
static void sjis_ascii(char *out, int n, const unsigned char *s, int len, int brk)
{
	static const unsigned short from[] = { 0x8140, 0x8143, 0x8144, 0x8145, 0x8146, 0x8147, 0x8148, 0x8149, 0x8151, 0x815B, 0x815C,
		0x815D, 0x815E, 0x8166, 0x8168, 0x8169, 0x816A, 0x816D, 0x816E, 0x817B, 0x817C, 0x8181, 0x8190, 0x8193, 0x8194,
		0x8195, 0x8196, 0x8197, 0x8165, 0x8167, 0x8142, 0x8141 };
	static const char to[] = " ,.-:;?!_---/'\"()[]+-=$%#&*@'\".,";
	int i = 0, o = 0;

	while (i < len && s[i] && o < n - 2)
	{
		int c = '?';

		if (i == brk && o && out[o - 1] != ' ')
			out[o++] = ' ';
		if (s[i] < 0x80)
			c = s[i] >= 0x20 && s[i] < 0x7F ? s[i] : ' ';
		else if (s[i] >= 0xA1 && s[i] <= 0xDF)
			c = '?';	/* half-width kana */
		else
		{
			unsigned code = ((unsigned)s[i] << 8) | s[i + 1];
			unsigned j;

			if (code >= 0x824F && code <= 0x8258)
				c = '0' + (int)(code - 0x824F);
			else if (code >= 0x8260 && code <= 0x8279)
				c = 'A' + (int)(code - 0x8260);
			else if (code >= 0x8281 && code <= 0x829A)
				c = 'a' + (int)(code - 0x8281);
			for (j = 0; j < sizeof from / sizeof from[0]; j++)
			{
				if (from[j] == code)
					c = to[j];
			}
			i++;
		}
		i++;
		if (c == ' ' && (!o || out[o - 1] == ' '))
			continue;
		out[o++] = (char)c;
	}
	while (o && out[o - 1] == ' ')
		o--;
	out[o] = 0;
}

static int has_letters(const char *s)
{
	for (; *s; s++)
	{
		if ((*s >= '0' && *s <= '9') || ((*s | 0x20) >= 'a' && (*s | 0x20) <= 'z'))
			return 1;
	}

	return 0;
}

/* ---- icons ---- */

static unsigned rd32(const unsigned char *p)
{
	return p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int rd16s(const unsigned char *p)
{
	return (short)(p[0] | (p[1] << 8));
}

static u32 px16(unsigned c)
{
	unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;

	return ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((b << 3) | (b >> 2)) << 16) | 0x80000000u;
}

static void icons_init(void)
{
	int i;

	for (i = 0; i < MC_ICONS; i++)
	{
		GSTEXTURE *t = &mc_icons[i].tex;

		t->Width = ICON_PX;
		t->Height = ICON_PX;
		t->PSM = GS_PSM_CT32;
		t->Filter = GS_FILTER_LINEAR;
		t->Delayed = 1;
		t->Mem = memalign(128, ICON_PX * ICON_PX * 4);
		mc_icons[i].port = -1;
	}
}

static void icons_forget(void)
{
	int i, p;

	for (i = 0; i < MC_ICONS; i++)
	{
		if (mc_icons[i].v)
			free(mc_icons[i].v);
		mc_icons[i].v = NULL;
		mc_icons[i].port = -1;
	}
	for (p = 0; p < 2; p++)
	{
		for (i = 0; i < MC_SAVES; i++)
		{
			if (mc_saves[p][i].icon >= 0)
				mc_saves[p][i].icon = -1;
		}
	}
}

/* An icon file into a slot. 0, or negative when the file is not what the format says. */
static int icon_parse(mcicon_t *ic, const unsigned char *d, int size)
{
	unsigned nsh, typ, nv, stride, i, keep, nframes, o;
	int lo[3] = { 32767, 32767, 32767 }, hi[3] = { -32768, -32768, -32768 }, a;
	u32 *px = (u32 *)ic->tex.Mem;
	float ext;

	if (size < 20 || rd32(d) != 0x10000)
		return -1;
	nsh = rd32(d + 4);
	typ = rd32(d + 8);
	nv = rd32(d + 16);
	if (nsh < 1 || nsh > 16 || nv < 3 || nv > 20000)
		return -2;
	stride = 8 * nsh + 16;
	if (20 + nv * stride + 20 > (unsigned)size)
		return -3;

	keep = nv > MC_VERTS ? MC_VERTS : nv;
	keep -= keep % 3;
	ic->v = malloc(keep * 8 * sizeof(short));
	if (!ic->v)
		return -4;
	for (i = 0; i < keep; i++)
	{
		const unsigned char *p = d + 20 + i * stride, *q = p + 8 * nsh;
		short *v = ic->v + i * 8;

		for (a = 0; a < 3; a++)
		{
			v[a] = (short)rd16s(p + 2 * a);
			v[3 + a] = (short)rd16s(q + 2 * a);
			if (v[a] < lo[a])
				lo[a] = v[a];
			if (v[a] > hi[a])
				hi[a] = v[a];
		}
		for (a = 0; a < 2; a++)
		{
			int t = rd16s(q + 8 + 2 * a) * ICON_PX / 4096;

			v[6 + a] = (short)(t < 0 ? 0 : t > ICON_PX ? ICON_PX : t);
		}
	}
	ic->nv = (int)keep;
	ic->cx = (lo[0] + hi[0]) * 0.5f;
	ic->cy = (lo[1] + hi[1]) * 0.5f;
	ic->cz = (lo[2] + hi[2]) * 0.5f;
	ext = (hi[0] - lo[0]) > (hi[2] - lo[2]) ? (float)(hi[0] - lo[0]) : (float)(hi[2] - lo[2]);
	ext *= 1.15f;		/* it turns: leave room for its corners */
	if ((float)(hi[1] - lo[1]) > ext)
		ext = (float)(hi[1] - lo[1]);
	ic->k = ext > 1.0f ? 2.0f / ext : 1.0f;

	/* the animation table: skipped, frame by frame */
	o = 20 + nv * stride;
	nframes = rd32(d + o + 16);
	o += 20;
	if (nframes > 256)
		goto bad;
	for (i = 0; i < nframes; i++)
	{
		unsigned nkeys;

		if (o + 16 > (unsigned)size)
			goto bad;
		nkeys = rd32(d + o + 4);
		if (nkeys < 1 || nkeys > 4096)
			goto bad;
		o += 16 + 8 * (nkeys - 1);
	}

	if (!(typ & 4))
	{
		for (i = 0; i < ICON_PX * ICON_PX; i++)
			px[i] = 0x80B0B0B0u;
	}
	else if (typ & 8)
	{
		unsigned end, n = 0;

		if (o + 4 > (unsigned)size)
			goto bad;
		end = o + 4 + rd32(d + o);
		o += 4;
		if (end > (unsigned)size)
			goto bad;
		while (o + 2 <= end && n < ICON_PX * ICON_PX)
		{
			unsigned code = d[o] | ((unsigned)d[o + 1] << 8), run;

			o += 2;
			if (code & 0x8000)
			{
				/* that many pixels follow as they are */
				for (run = 0x10000 - code; run && o + 2 <= end && n < ICON_PX * ICON_PX; run--, o += 2)
					px[n++] = px16(d[o] | ((unsigned)d[o + 1] << 8));
			}
			else
			{
				u32 c;

				if (o + 2 > end)
					break;
				c = px16(d[o] | ((unsigned)d[o + 1] << 8));
				o += 2;
				for (run = code; run && n < ICON_PX * ICON_PX; run--)
					px[n++] = c;
			}
		}
		if (n != ICON_PX * ICON_PX)
			goto bad;
	}
	else
	{
		if (o + ICON_PX * ICON_PX * 2 > (unsigned)size)
			goto bad;
		for (i = 0; i < ICON_PX * ICON_PX; i++)
			px[i] = px16(d[o + 2 * i] | ((unsigned)d[o + 2 * i + 1] << 8));
	}

	return 0;

bad:
	free(ic->v);
	ic->v = NULL;

	return -5;
}

static float mc_tx[MC_VERTS], mc_ty[MC_VERTS], mc_tz[MC_VERTS];
static unsigned char mc_tl[MC_VERTS];
static unsigned short mc_ord[MC_VERTS / 3];
static float mc_key[MC_VERTS / 3];

/* The model, turned (turn in turns) about its upright axis and tipped a little toward the
 * viewer, fitted into a box of the given size around (px, py). The viewer is on the -z side;
 * y grows downward in the file as on the screen. al = 0..0x80. */
static void icon_draw(mcicon_t *ic, float px, float py, float size, float turn, int al)
{
	const float st = 0.2079f, ct = 0.9781f;		/* 12 degrees */
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
		/* light from the viewer's side, a little from above and from the left */
		l = 0.74f - (nz * ct - ny * st) * (0.30f / 4096.0f) - (ny * ct + nz * st) * (0.14f / 4096.0f) - nx * (0.08f / 4096.0f);
		if (l < 0.50f)
			l = 0.50f;
		if (l > 1.0f)
			l = 1.0f;
		mc_tl[i] = (unsigned char)(l * 128.0f);
	}
	for (i = 0; i < n; i++)
	{
		mc_ord[i] = (unsigned short)i;
		mc_key[i] = mc_tz[3 * i] + mc_tz[3 * i + 1] + mc_tz[3 * i + 2];
	}
	/* far first */
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

	gsKit_TexManager_bind(gs, &ic->tex);
	for (i = 0; i < n; i++)
	{
		int a = 3 * mc_ord[i], b = a + 1, d = a + 2;
		const short *va = ic->v + a * 8, *vb = ic->v + b * 8, *vd = ic->v + d * 8;

		gsKit_prim_triangle_goraud_texture(gs, &ic->tex,
			mc_tx[a], mc_ty[a], (float)va[6], (float)va[7],
			mc_tx[b], mc_ty[b], (float)vb[6], (float)vb[7],
			mc_tx[d], mc_ty[d], (float)vd[6], (float)vd[7], 2,
			RGBA(mc_tl[a], mc_tl[a], mc_tl[a], al), RGBA(mc_tl[b], mc_tl[b], mc_tl[b], al), RGBA(mc_tl[d], mc_tl[d], mc_tl[d], al));
	}
	/* a model can be a thousand triangles: send them now, the frame's list is not endless */
	gsKit_queue_exec(gs);
}

/* ---- reading the icons, a step per frame so that the screen keeps moving ---- */

static struct
{
	int state;		/* 0 idle, 1 opening, 2 reading, 3 closing */
	int port, save, fd, got;
	unsigned size;
	unsigned char *buf;
} ld;
static int mc_want_port = -1, mc_want_save;

static void mc_ld_step(int block)
{
	int cmd = 0, res = -1, i, best = 0;
	save_t *s;
	char path[80];

	if (!ld.state)
	{
		if (block || mc_want_port < 0 || !mc_up)
			return;
		ld.port = mc_want_port;
		ld.save = mc_want_save;
		mc_want_port = -1;
		s = &mc_saves[ld.port][ld.save];
		if (s->icon != -1)
			return;
		if (!s->iconfile[0] || s->iconsize < 64 || s->iconsize > 0x80000)
		{
			s->icon = -2;
			return;
		}
		ld.size = s->iconsize;
		ld.buf = memalign(64, (ld.size + 127) & ~63u);
		if (!ld.buf)
		{
			s->icon = -2;
			return;
		}
		snprintf(path, sizeof path, "/%s/%s", s->dir, s->iconfile);
		if (mcOpen(ld.port, 0, path, 1) < 0)
		{
			free(ld.buf);
			s->icon = -2;
			return;
		}
		ld.state = 1;

		return;
	}

	if (mcSync(block ? MC_WAIT : MC_NOWAIT, &cmd, &res) == 0)
		return;		/* still at it */
	s = &mc_saves[ld.port][ld.save];
	if (ld.state == 1)
	{
		if (res < 0)
		{
			free(ld.buf);
			s->icon = -2;
			ld.state = 0;
			return;
		}
		ld.fd = res;
		mcRead(ld.fd, ld.buf, (int)ld.size);
		ld.state = 2;
	}
	else if (ld.state == 2)
	{
		ld.got = res;
		mcClose(ld.fd);
		ld.state = 3;
	}
	else
	{
		ld.state = 0;
		s->icon = -2;
		if (ld.got == (int)ld.size)
		{
			/* a free slot, or the one not drawn for longest */
			for (i = 1; i < MC_ICONS; i++)
			{
				if (mc_icons[best].port >= 0 && (mc_icons[i].port < 0 || mc_icons[i].used < mc_icons[best].used))
					best = i;
			}
			if (mc_icons[best].port >= 0)
				mc_saves[mc_icons[best].port][mc_icons[best].save].icon = -1;
			if (mc_icons[best].v)
				free(mc_icons[best].v);
			mc_icons[best].v = NULL;
			mc_icons[best].port = -1;
			if (icon_parse(&mc_icons[best], ld.buf, (int)ld.size) == 0)
			{
				mc_icons[best].port = ld.port;
				mc_icons[best].save = ld.save;
				mc_icons[best].used = frame;
				gsKit_TexManager_invalidate(gs, &mc_icons[best].tex);
				s->icon = (signed char)best;
			}
		}
		free(ld.buf);
	}
}

/* No card call may start while an icon is being read. */
static void mc_ld_finish(void)
{
	while (ld.state)
		mc_ld_step(1);
	mc_want_port = -1;
}

/* ---- the cards ---- */

static void mc_scan(int port)
{
	int type = 0, fr = 0, fmt = 0, r, n, i, f, nf;
	char path[80];

	mc_n[port] = 0;
	mc_card[port] = 0;
	mc_free[port] = 0;
	if (!mc_up)
		return;
	mcGetInfo(port, 0, &type, &fr, &fmt);
	r = mc_wait();
	if (r < -2 || type != MC_TYPE_PS2)
		return;
	if (r == -2 || !fmt)
	{
		mc_card[port] = -2;
		return;
	}
	mc_card[port] = 1;
	mc_free[port] = fr;

	mcGetDir(port, 0, "/*", 0, MC_SAVES, mc_tab);
	n = mc_wait();
	for (i = 0; i < n && mc_n[port] < MC_SAVES; i++)
	{
		save_t *s = &mc_saves[port][mc_n[port]];
		const char *name = (const char *)mc_tab[i].EntryName;

		if (!(mc_tab[i].AttrFile & MC_ATTR_SUBDIR) || !name[0] || name[0] == '.' || strchr(name, '/'))
			continue;
		memset(s, 0, sizeof *s);
		strncpy(s->dir, name, 32);
		s->when = mc_tab[i]._Modify;
		s->icon = -1;
		mc_n[port]++;
	}

	for (i = 0; i < mc_n[port]; i++)
	{
		save_t *s = &mc_saves[port][i];
		int fd, got = 0, brk;

		snprintf(path, sizeof path, "/%s/*", s->dir);
		mcGetDir(port, 0, path, 0, MC_FILES, mc_ftab);
		nf = mc_wait();
		for (f = 0; f < nf; f++)
		{
			if (mc_ftab[f].AttrFile & MC_ATTR_SUBDIR)
				continue;
			s->bytes += mc_ftab[f].FileSizeByte;
			s->nfiles++;
		}

		strcpy(s->title, s->dir);
		snprintf(path, sizeof path, "/%s/icon.sys", s->dir);
		mcOpen(port, 0, path, 1);
		fd = mc_wait();
		if (fd >= 0)
		{
			mcRead(fd, mc_sys, 964);
			got = mc_wait();
			mcClose(fd);
			mc_wait();
		}
		if (got < 964 || memcmp(mc_sys, "PS2D", 4))
		{
			s->icon = -2;
			continue;
		}
		brk = mc_sys[6] | (mc_sys[7] << 8);
		sjis_ascii(s->title, sizeof s->title, mc_sys + 0xC0, 68, brk);
		if (!has_letters(s->title))
			strcpy(s->title, s->dir);
		memcpy(s->iconfile, mc_sys + 0x104, 32);
		s->iconfile[32] = 0;
		for (f = 0; f < nf; f++)
		{
			if (!(mc_ftab[f].AttrFile & MC_ATTR_SUBDIR) && !strcmp((const char *)mc_ftab[f].EntryName, s->iconfile))
				s->iconsize = mc_ftab[f].FileSizeByte;
		}
	}
}

static void mc_rescan(void)
{
	mc_ld_finish();
	icons_forget();
	mc_scan(0);
	mc_scan(1);
	if (mc_sel >= mc_n[mc_port])
		mc_sel = mc_n[mc_port] ? mc_n[mc_port] - 1 : 0;
	mc_seen = 1;
}

static int mc_kb(unsigned bytes)
{
	return (int)((bytes + 1023) / 1024);
}

/* Remove one save folder with what is in it. 0 = gone. */
static int mc_remove(int port, const char *dir)
{
	char path[80];
	int nf, f, r;

	if (!dir[0] || strchr(dir, '/') || dir[0] == '.')
		return -100;		/* never a path that is not one save's folder */
	snprintf(path, sizeof path, "/%s/*", dir);
	mcGetDir(port, 0, path, 0, MC_FILES, mc_ftab);
	nf = mc_wait();
	for (f = 0; f < nf; f++)
	{
		const char *name = (const char *)mc_ftab[f].EntryName;

		if (!strcmp(name, ".") || !strcmp(name, ".."))
			continue;
		if ((mc_ftab[f].AttrFile & MC_ATTR_SUBDIR) || !name[0] || strchr(name, '/'))
			return -101;	/* a folder inside a save: left alone */
		snprintf(path, sizeof path, "/%s/%s", dir, name);
		mcDelete(port, 0, path);
		r = mc_wait();
		if (r < 0)
			return r;
	}
	snprintf(path, sizeof path, "/%s", dir);
	mcDelete(port, 0, path);

	return mc_wait();
}

static void mc_do_delete(int port, int si)
{
	save_t *s = &mc_saves[port][si];
	char title[72], msg[110];
	int r;

	strcpy(title, s->title);
	mc_ld_finish();
	progress(NULL, "DELETING", title, 0.5f, "Do not switch the console off");
	r = mc_remove(port, s->dir);
	mc_rescan();
	if (r < 0)
		snprintf(msg, sizeof msg, "Could not delete it (code %d).", r);
	else
		snprintf(msg, sizeof msg, "Deleted: %s", title);
	say(msg);
}

static void mc_do_copy(int sp, int si)
{
	save_t *s = &mc_saves[sp][si];
	int dp = 1 - sp, nf, f, r = 0, i, done = 0, need, made = 0;
	char dir[33], title[72], path[80], msg[110];
	unsigned char *buf = NULL, *back = NULL;

	strcpy(dir, s->dir);
	strcpy(title, s->title);
	mc_ld_finish();
	if (mc_card[dp] != 1)
	{
		say(dp ? "There is no memory card in slot 2." : "There is no memory card in slot 1.");
		return;
	}
	for (i = 0; i < mc_n[dp]; i++)
	{
		if (!strcmp(mc_saves[dp][i].dir, dir))
		{
			say("That save is already on the other card. Delete it there first.");
			return;
		}
	}
	need = mc_kb(s->bytes) + 2 * s->nfiles + 4;
	if (mc_free[dp] < need)
	{
		say("Not enough room on the other card.");
		return;
	}

	progress(NULL, "COPYING", title, 0.0f, "Do not switch the console off");
	/* the folder's own entry (its dates), then its files */
	mcGetDir(sp, 0, "/*", 0, MC_SAVES, mc_tab);
	nf = mc_wait();
	memset(&mc_dinfo, 0, sizeof mc_dinfo);
	for (i = 0; i < nf; i++)
	{
		if (!strcmp((const char *)mc_tab[i].EntryName, dir))
			mc_dinfo = mc_tab[i];
	}
	snprintf(path, sizeof path, "/%s/*", dir);
	mcGetDir(sp, 0, path, 0, MC_FILES, mc_ftab);
	nf = mc_wait();
	if (nf < 0)
		r = nf;

	if (r >= 0)
	{
		snprintf(path, sizeof path, "/%s", dir);
		mcMkDir(dp, 0, path);
		r = mc_wait();
		made = r >= 0;
	}
	for (f = 0; r >= 0 && f < nf; f++)
	{
		const char *name = (const char *)mc_ftab[f].EntryName;
		int size = (int)mc_ftab[f].FileSizeByte, fd, got;

		if (!strcmp(name, ".") || !strcmp(name, ".."))
			continue;
		if ((mc_ftab[f].AttrFile & MC_ATTR_SUBDIR) || !name[0] || strchr(name, '/'))
		{
			r = -101;
			break;
		}
		buf = memalign(64, ((unsigned)size + 127) & ~63u);
		back = memalign(64, ((unsigned)size + 127) & ~63u);
		if (!buf || !back)
		{
			r = -102;
			break;
		}
		snprintf(path, sizeof path, "/%s/%s", dir, name);

		mcOpen(sp, 0, path, 1);
		fd = mc_wait();
		if (fd < 0)
		{
			r = fd;
			break;
		}
		got = 0;
		if (size)
		{
			mcRead(fd, buf, size);
			got = mc_wait();
		}
		mcClose(fd);
		mc_wait();
		if (got != size)
		{
			r = -103;
			break;
		}

		mcOpen(dp, 0, path, 2 | 0x200);		/* write, create */
		fd = mc_wait();
		if (fd < 0)
		{
			r = fd;
			break;
		}
		got = 0;
		if (size)
		{
			mcWrite(fd, buf, size);
			got = mc_wait();
		}
		mcClose(fd);
		i = mc_wait();
		if (got != size || i < 0)
		{
			r = -104;
			break;
		}

		/* read the copy back: it must be the same bytes */
		mcOpen(dp, 0, path, 1);
		fd = mc_wait();
		if (fd < 0)
		{
			r = -105;
			break;
		}
		got = 0;
		if (size)
		{
			mcRead(fd, back, size);
			got = mc_wait();
		}
		mcClose(fd);
		mc_wait();
		if (got != size || memcmp(buf, back, (unsigned)size))
		{
			r = -106;
			break;
		}
		mcSetFileInfo(dp, 0, path, &mc_ftab[f], 1 | 2 | 4);	/* its dates and attributes */
		mc_wait();

		free(buf);
		free(back);
		buf = back = NULL;
		done += size;
		progress(NULL, "COPYING", title, s->bytes ? (float)done / (float)s->bytes : 1.0f, "Do not switch the console off");
	}
	if (buf)
		free(buf);
	if (back)
		free(back);

	if (r >= 0 && mc_dinfo.EntryName[0])
	{
		snprintf(path, sizeof path, "/%s", dir);
		mcSetFileInfo(dp, 0, path, &mc_dinfo, 1 | 2 | 4);
		mc_wait();
	}
	if (r < 0 && made)
		mc_remove(dp, dir);		/* no half copy is left behind */
	mc_rescan();
	if (r < 0)
		snprintf(msg, sizeof msg, "Could not copy it (code %d). Nothing was changed.", r);
	else
		snprintf(msg, sizeof msg, "Copied to slot %d: %s", dp + 1, title);
	say(msg);
}

/* ---- drawing ---- */

static void sheet(float x0, float y0, float x1, float y1)
{
	rect(0.0f, 0.0f, 640.0f, 448.0f, RGBA(0x02, 0x04, 0x0C, 0x58));
	rect(x0, y0, x1, y1, RGBA(0x0D, 0x14, 0x33, 0x80));
	frame_rect(x0, y0, x1, y1, 1.0f, LINE);
	spectrum_bar(x0, x1, y0, 2.0f, 6, 0x80);
}

static void sheet_item(int y, int chosen, const char *s)
{
	if (chosen)
	{
		rect(196.0f, (float)y - 5.0f, 444.0f, (float)y + 23.0f, RGBA(0x7F, 0xE3, 0xFF, 0x1C));
		frame_rect(196.0f, (float)y - 5.0f, 444.0f, (float)y + 23.0f, 1.0f, GLOW);
	}
	text_centre(F_BODY, 320, y, 0, chosen ? INK : DIM, s);
}

static void draw_mc(void)
{
	char line[110], t[80];
	int p = mc_port, r, c, x;
	save_t *s = mc_n[p] ? &mc_saves[p][mc_sel] : NULL;

	top_bar("MEMORY CARD");
	for (c = 0; c < 2; c++)
	{
		snprintf(line, sizeof line, "SLOT %d", c + 1);
		text_ex(F_SMALL, LEFT + c * 86, 74, 1.0f, 3, c == p ? INK : FAINT, line);
		if (c == p)
			spectrum_bar((float)(LEFT + c * 86), (float)(LEFT + c * 86 + 58), 92.0f, 2.0f, 3, 0x80);
	}
	if (mc_card[p] == 1)
	{
		snprintf(line, sizeof line, "%d SAVE%s", mc_n[p], mc_n[p] == 1 ? "" : "S");
		snprintf(t, sizeof t, "  " DOT "  %d KB FREE", mc_free[p]);
		top_right(line, DIM, t, FAINT);
	}

	if (mc_card[p] != 1)
	{
		text_centre(F_HEAD, 320, 176, 0, INK, mc_card[p] == -2 ? "Card not formatted" : "No memory card");
		snprintf(line, sizeof line, "Slot %d has no PS2 memory card.", p + 1);
		text_centre(F_BODY, 320, 222, 0, DIM, mc_card[p] == -2 ? "This card has not been formatted for the PS2." : line);
		text_centre(F_BODY, 320, 248, 0, DIM, "Cards are chosen on the XMB, under Memory Card Utility.");
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

				if (i >= mc_n[p])
					continue;
				e = &mc_saves[p][i];
				if (chosen)
				{
					/* the light it stands in */
					gsKit_prim_quad_gouraud(gs, cx - 58.0f, cy + 46.0f, cx + 58.0f, cy + 46.0f, cx - 34.0f, cy + 54.0f,
						cx + 34.0f, cy + 54.0f, 1, RGBA(0x7F, 0xE3, 0xFF, 0x30), RGBA(0x7F, 0xE3, 0xFF, 0x30),
						RGBA(0x7F, 0xE3, 0xFF, 0x00), RGBA(0x7F, 0xE3, 0xFF, 0x00));
				}
				if (e->icon >= 0)
				{
					mc_icons[e->icon].used = frame;
					icon_draw(&mc_icons[e->icon], cx, cy + bob, size, turn, chosen ? 0x80 : 0x74);
				}
				else
				{
					/* not read yet, or it has none: a turning frame */
					float w = size * 0.30f * (0.35f + 0.65f * (fcos(turn * 3.0f) < 0.0f ? -fcos(turn * 3.0f) : fcos(turn * 3.0f)));

					frame_rect(cx - w, cy + bob - size * 0.30f, cx + w, cy + bob + size * 0.30f, 2.0f, chosen ? GLOW : FAINT);
					if (e->icon == -1 && mc_want_port < 0)
					{
						mc_want_port = p;
						mc_want_save = i;
					}
				}
			}
		}
		gsKit_TexManager_bind(gs, &ftex);

		fit(line, sizeof line, F_BOLD, s->title, RIGHT - LEFT);
		text_centre(F_BOLD, 320, 318, 0, INK, line);
		snprintf(line, sizeof line, "%s  " DOT "  %d KB  " DOT "  %04d-%02d-%02d", s->dir, mc_kb(s->bytes), s->when.Year, s->when.Month, s->when.Day);
		fit(t, sizeof t, F_SMALL, line, RIGHT - LEFT);
		text_centre(F_SMALL, 320, 346, 1, DIM, t);
		snprintf(line, sizeof line, "%d OF %d", mc_sel + 1, mc_n[p]);
		text_centre(F_SMALL, 320, 364, 1, FAINT, line);
	}
	if (note_frames > 0)
		text_centre(F_SMALL, 320, 380, 0, AMBER, note);

	legend_line();
	x = LEFT;
	if (s)
		x = legend_item(x, IC_CROSS, NULL, "Copy or delete");
	x = legend_item(x, -1, "L1 R1", "Slot");
	legend_item(x, IC_CIRCLE, NULL, "Menu");

	if (mc_mode == MM_OPTIONS && s)
	{
		sheet(176.0f, 138.0f, 464.0f, 304.0f);
		fit(line, sizeof line, F_SMALL, s->title, 260);
		text_centre(F_SMALL, 320, 152, 1, DIM, line);
		snprintf(line, sizeof line, "Copy to slot %d", 2 - p);
		sheet_item(186, mc_pick == 0, line);
		sheet_item(222, mc_pick == 1, "Delete");
		sheet_item(258, mc_pick == 2, "Cancel");
	}
	else if ((mc_mode == MM_ASK_COPY || mc_mode == MM_ASK_DELETE) && s)
	{
		sheet(150.0f, 138.0f, 490.0f, 304.0f);
		text_centre(F_BOLD, 320, 154, 0, INK, mc_mode == MM_ASK_COPY ? "Copy this save?" : "Delete this save?");
		fit(line, sizeof line, F_BODY, s->title, 310);
		text_centre(F_BODY, 320, 188, 0, DIM, line);
		if (mc_mode == MM_ASK_COPY)
			snprintf(line, sizeof line, "It will be copied to the card in slot %d.", 2 - p);
		else
			snprintf(line, sizeof line, "It will be gone from slot %d for good.", p + 1);
		text_centre(F_SMALL, 320, 216, 0, mc_mode == MM_ASK_COPY ? DIM : AMBER, line);
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

/* a memory card, drawn: for the HOME tile */
static void card_shape(float cx, float cy, float w, float h, int al)
{
	u64 body = RGBA(0x2A, 0x36, 0x6E, al), edge = RGBA(0x7F, 0xE3, 0xFF, al * 0x60 / 0x80);

	rect(cx - w, cy - h, cx + w, cy + h, body);
	rect(cx - w * 0.72f, cy - h * 0.78f, cx + w * 0.72f, cy - h * 0.30f, RGBA(0xE9, 0xED, 0xFF, al * 0x30 / 0x80));
	rect(cx - w * 0.5f, cy + h * 0.55f, cx + w * 0.5f, cy + h * 0.80f, RGBA(0x06, 0x0A, 0x1A, al));
	frame_rect(cx - w, cy - h, cx + w, cy + h, 2.0f, edge);
	spectrum_bar(cx - w * 0.72f, cx + w * 0.72f, cy - h * 0.10f, 3.0f, 4, al);
}

static void draw_home(void)
{
	static const char *const name[3] = { "PS2", "PS1", "MEMORY CARD" };
	char sub[3][40];
	int i, x;

	if (st.ok && n_games)
		snprintf(sub[0], sizeof sub[0], "%d game%s on the card", n_games, n_games == 1 ? "" : "s");
	else
		strcpy(sub[0], "No games found");
	strcpy(sub[1], "Not available yet");
	strcpy(sub[2], "Copy and delete saves");

	top_bar("PRISM");
	text_centre(F_SMALL, 320, 92, 5, DIM, "WHERE TO?");
	for (i = 0; i < 3; i++)
	{
		int chosen = i == home_sel, al = chosen ? 0x80 : 0x48;
		float cx = 126.0f + i * 194.0f, lift = chosen ? 6.0f * fsin(frame / 200.0f) - 6.0f : 0.0f;
		float cy = 208.0f + lift;

		rect(cx - 91.0f, cy - 86.0f, cx + 91.0f, cy + 96.0f, chosen ? RGBA(0x14, 0x1E, 0x4A, 0x78) : RGBA(0x0D, 0x14, 0x33, 0x60));
		frame_rect(cx - 91.0f, cy - 86.0f, cx + 91.0f, cy + 96.0f, chosen ? 2.0f : 1.0f, chosen ? GLOW : LINE);
		if (chosen)
			spectrum_bar(cx - 91.0f, cx + 91.0f, cy + 94.0f, 2.0f, 4, 0x80);
		if (i == 2)
			card_shape(cx, cy - 26.0f, 30.0f, 40.0f, al);
		else
		{
			disc(cx, cy - 26.0f, 44.0f, 11.0f, i == 1 ? al / 2 : al);
			frame_rect(cx - 5.0f, cy - 31.0f, cx + 5.0f, cy - 21.0f, 1.0f, RGBA(0xE9, 0xED, 0xFF, al / 2));
		}
		text_centre(F_BOLD, (int)cx, (int)cy + 34, i == 2 ? 0 : 4, chosen ? INK : DIM, name[i]);
		text_centre(F_SMALL, (int)cx, (int)cy + 64, 0, chosen ? DIM : FAINT, sub[i]);
	}
	if (note_frames > 0)
		text_centre(F_SMALL, 320, 352, 0, AMBER, note);

	legend_line();
	x = legend_item(LEFT, IC_CROSS, NULL, "Open");
	(void)x;
	if (st.ver[0])
		text(F_SMALL, RIGHT - text_w(F_SMALL, st.ver, 0), 404, FAINT, st.ver);
}

static void draw_ps1(void)
{
	top_bar("PS1");
	text_centre(F_HEAD, 320, 150, 0, INK, "Not available yet");
	text_centre(F_BODY, 320, 206, 0, DIM, "PlayStation games run in a different emulator,");
	text_centre(F_BODY, 320, 232, 0, DIM, "which only the XMB can start.");
	text_centre(F_SMALL, 320, 280, 0, FAINT, "For now, start PS1 games from the XMB as before.");
	legend_line();
	legend_item(LEFT, IC_CIRCLE, NULL, "Menu");
}

/* ---- pad ---- */

static void mc_open(void)
{
	in_mc = 1;
	at_home = 0;
	mc_mode = MM_BROWSE;
	progress(NULL, "ONE MOMENT", "Reading the memory cards", 0.5f, "");
	mc_rescan();
	mc_port = mc_card[0] == 1 || mc_card[1] != 1 ? 0 : 1;
	mc_sel = mc_top = 0;
}

static void mc_pad(unsigned pressed)
{
	int n = mc_n[mc_port];

	if (mc_mode == MM_OPTIONS)
	{
		if (pressed & PAD_UP)
			mc_pick = mc_pick ? mc_pick - 1 : 2;
		if (pressed & PAD_DOWN)
			mc_pick = mc_pick < 2 ? mc_pick + 1 : 0;
		if (pressed & (PAD_CIRCLE | PAD_TRIANGLE))
			mc_mode = MM_BROWSE;
		else if (pressed & PAD_CROSS)
		{
			mc_mode = mc_pick == 0 ? MM_ASK_COPY : mc_pick == 1 ? MM_ASK_DELETE : MM_BROWSE;
			mc_pick = 0;		/* the question starts on "No" */
		}
		return;
	}
	if (mc_mode == MM_ASK_COPY || mc_mode == MM_ASK_DELETE)
	{
		if (pressed & (PAD_LEFT | PAD_RIGHT))
			mc_pick ^= 1;
		if (pressed & (PAD_CIRCLE | PAD_TRIANGLE))
			mc_mode = MM_BROWSE;
		else if (pressed & PAD_CROSS)
		{
			int mode = mc_mode;

			mc_mode = MM_BROWSE;
			if (mc_pick == 1 && n)
			{
				if (mode == MM_ASK_COPY)
					mc_do_copy(mc_port, mc_sel);
				else
					mc_do_delete(mc_port, mc_sel);
			}
		}
		return;
	}

	if (pressed & (PAD_CIRCLE | PAD_TRIANGLE))
	{
		mc_ld_finish();
		in_mc = 0;
		at_home = 1;
		return;
	}
	if (pressed & (PAD_L1 | PAD_R1))
	{
		mc_port ^= 1;
		mc_sel = mc_top = 0;
	}
	if (pressed & PAD_SELECT)
	{
		progress(NULL, "ONE MOMENT", "Reading the memory cards", 0.5f, "");
		mc_rescan();
	}
	n = mc_n[mc_port];
	if (!n)
		return;
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
		mc_mode = MM_OPTIONS;
		mc_pick = 2;		/* starts on "Cancel" */
	}
}

static void home_pad(unsigned pressed)
{
	if (pressed & PAD_LEFT)
		home_sel = home_sel ? home_sel - 1 : 2;
	if (pressed & PAD_RIGHT)
		home_sel = home_sel < 2 ? home_sel + 1 : 0;
	if (pressed & PAD_CROSS)
	{
		if (home_sel == 0)
			at_home = 0;
		else if (home_sel == 1)
		{
			at_home = 0;
			in_ps1 = 1;
		}
		else if (mc_up)
			mc_open();
		else
			say("The memory card modules did not load.");
	}
}
