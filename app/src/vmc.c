/* Reading the PS3's virtual memory cards. PS2 layout as launcher/mc_extract.py (checked there
 * against the console's own listing); icon reading as launcher/ui/mcard.h icon_parse (checked by
 * icon_check.py against the owner's nine real icons). PS1 layout: psx-spx "Memory Card Data
 * Format". Read-only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __PPU__
#include <lv2/sysfs.h>
#endif
#include "vmc.h"

card_t cards[MC_CARDS];
int n_cards;

static u32 le32(const u8 *p)
{
	return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u32 le16(const u8 *p)
{
	return p[0] | ((u32)p[1] << 8);
}

static int rd16s(const u8 *p)
{
	return (short)(p[0] | (p[1] << 8));
}

/* ------------------------------------------------------------------ names */

/* icon.sys / PS1 titles are Shift-JIS, nearly always full-width Latin (mcard.h sjis_ascii) */
static void sjis_ascii(char *out, int n, const u8 *s, int len, int brk)
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
			c = '?';
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
				if (from[j] == code)
					c = to[j];
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
		if ((*s >= '0' && *s <= '9') || ((*s | 0x20) >= 'a' && (*s | 0x20) <= 'z'))
			return 1;
	return 0;
}

/* ------------------------------------------------------------------ PS2 cards */

static const u8 *page_p(const card_t *c, int p)
{
	return c->raw + (long)p * (c->page + c->spare);
}

static void read_cluster(const card_t *c, u32 n, u8 *out)
{
	int k;

	for (k = 0; k < c->ppc; k++)
	{
		long p = (long)n * c->ppc + k;

		if ((p + 1) * (c->page + c->spare) > c->len)
			memset(out + k * c->page, 0, c->page);
		else
			memcpy(out + k * c->page, page_p(c, (int)p), c->page);
	}
}

static u32 fat_entry(const card_t *c, u32 n)
{
	u8 b[8192];
	u32 per = c->cl / 4, fatcl;

	if (n / (per * per) >= 32)
		return 0xFFFFFFFF;
	read_cluster(c, c->ifc[n / (per * per)], b);
	fatcl = le32(b + 4 * ((n / per) % per));
	read_cluster(c, fatcl, b);
	return le32(b + 4 * (n % per));
}

/* the clusters of a chain, cut to length; malloc'd */
static u8 *chain(const card_t *c, u32 first, u32 length)
{
	u8 *out = (u8 *)malloc(length + c->cl);
	u32 got = 0, n = first, seen = 0;

	if (!out)
		return NULL;
	while (got < length)
	{
		u32 e;

		read_cluster(c, c->alloc_offset + n, out + got);
		got += c->cl;
		e = fat_entry(c, n);
		if (e == 0xFFFFFFFF || !(e & 0x80000000) || ++seen > 20000)
			break;
		n = e & 0x7FFFFFFF;
	}
	if (got < length)
		memset(out + got, 0, length - got);
	return out;
}

/* a directory's entries (512 bytes each), count = the directory's own length field */
static u8 *dir_entries(const card_t *c, u32 first, u32 count)
{
	return chain(c, first, count * 512);
}

static int ps2_icon(save_t *s, const u8 *d, u32 size)
{
	u32 nsh, typ, nv, stride, i, keep, nframes, o;
	int lo[3] = { 32767, 32767, 32767 }, hi[3] = { -32768, -32768, -32768 }, a;
	float ext;

	if (size < 20 || le32(d) != 0x10000)
		return -1;
	nsh = le32(d + 4);
	typ = le32(d + 8);
	nv = le32(d + 16);
	if (nsh < 1 || nsh > 16 || nv < 3 || nv > 20000)
		return -2;
	stride = 8 * nsh + 16;
	if (20 + nv * stride + 20 > size)
		return -3;
	keep = nv > MC_VERTS ? MC_VERTS : nv;
	keep -= keep % 3;
	s->v = (short *)malloc(keep * 8 * sizeof(short));
	s->px = (u8 *)malloc(ICON_PX * ICON_PX * 4);
	if (!s->v || !s->px)
		return -4;
	for (i = 0; i < keep; i++)
	{
		const u8 *p = d + 20 + i * stride, *q = p + 8 * nsh;
		short *v = s->v + i * 8;

		for (a = 0; a < 3; a++)
		{
			v[a] = (short)rd16s(p + 2 * a);
			v[3 + a] = (short)rd16s(q + 2 * a);
			if (v[a] < lo[a]) lo[a] = v[a];
			if (v[a] > hi[a]) hi[a] = v[a];
		}
		for (a = 0; a < 2; a++)
		{
			int t = rd16s(q + 8 + 2 * a) * ICON_PX / 4096;

			v[6 + a] = (short)(t < 0 ? 0 : t > ICON_PX ? ICON_PX : t);
		}
	}
	s->nv = (int)keep;
	s->cx = (lo[0] + hi[0]) * 0.5f;
	s->cy = (lo[1] + hi[1]) * 0.5f;
	s->cz = (lo[2] + hi[2]) * 0.5f;
	ext = (hi[0] - lo[0]) > (hi[2] - lo[2]) ? (float)(hi[0] - lo[0]) : (float)(hi[2] - lo[2]);
	ext *= 1.15f;
	if ((float)(hi[1] - lo[1]) > ext)
		ext = (float)(hi[1] - lo[1]);
	s->k = ext > 1.0f ? 2.0f / ext : 1.0f;

	o = 20 + nv * stride;
	nframes = le32(d + o + 16);
	o += 20;
	if (nframes > 256)
		return -5;
	for (i = 0; i < nframes; i++)
	{
		u32 nkeys;

		if (o + 16 > size)
			return -5;
		nkeys = le32(d + o + 4);
		if (nkeys < 1 || nkeys > 4096)
			return -5;
		o += 16 + 8 * (nkeys - 1);
	}

#define PX16(cv, out) do { unsigned r_ = (cv) & 31, g_ = ((cv) >> 5) & 31, b_ = ((cv) >> 10) & 31; \
	(out)[0] = (u8)((r_ << 3) | (r_ >> 2)); (out)[1] = (u8)((g_ << 3) | (g_ >> 2)); (out)[2] = (u8)((b_ << 3) | (b_ >> 2)); (out)[3] = 0x80; } while (0)

	if (!(typ & 4))
	{
		for (i = 0; i < ICON_PX * ICON_PX; i++)
		{
			s->px[4 * i] = s->px[4 * i + 1] = s->px[4 * i + 2] = 0xB0;
			s->px[4 * i + 3] = 0x80;
		}
	}
	else if (typ & 8)
	{
		u32 end, n = 0;

		if (o + 4 > size)
			return -6;
		end = o + 4 + le32(d + o);
		o += 4;
		if (end > size)
			return -6;
		while (o + 2 <= end && n < ICON_PX * ICON_PX)
		{
			u32 code = le16(d + o), run;

			o += 2;
			if (code & 0x8000)
			{
				for (run = 0x10000 - code; run && o + 2 <= end && n < ICON_PX * ICON_PX; run--, o += 2, n++)
					PX16(le16(d + o), s->px + 4 * n);
			}
			else
			{
				u8 c4[4];

				if (o + 2 > end)
					break;
				PX16(le16(d + o), c4);
				o += 2;
				for (run = code; run && n < ICON_PX * ICON_PX; run--, n++)
					memcpy(s->px + 4 * n, c4, 4);
			}
		}
		if (n != ICON_PX * ICON_PX)
			return -6;
	}
	else
	{
		if (o + ICON_PX * ICON_PX * 2 > size)
			return -6;
		for (i = 0; i < ICON_PX * ICON_PX; i++)
			PX16(le16(d + o + 2 * i), s->px + 4 * i);
	}
	s->tex.w = ICON_PX;
	s->tex.h = ICON_PX;
	s->tex.px = s->px;
	s->icon = 1;
	return 0;
}

static void fmt_when(char *out, const u8 *t)
{
	/* sceMcStDateTime: resv, sec, min, hour, day, month, year (u16) */
	unsigned y = le16(t + 6);

	if (y >= 1990 && y < 2100 && t[5] >= 1 && t[5] <= 12 && t[4] >= 1 && t[4] <= 31)
		snprintf(out, 20, "%04u-%02u-%02u", y, t[5], t[4]);
	else
		out[0] = 0;
}

static int load_ps2(card_t *c)
{
	const u8 *h = c->raw;
	u8 *root, *e0;
	u32 rootlen, i, k;
	int n;

	if (c->len < 0x200 || memcmp(h, "Sony PS2 Memory Card Format ", 28))
		return -1;
	c->page = (int)le16(h + 0x28);
	c->ppc = (int)le16(h + 0x2A);
	if (c->page != 512 || c->ppc < 1 || c->ppc > 16)
		return -2;
	n = (int)(c->len / (c->page + 16));
	c->spare = (c->len % (c->page + 16) == 0 && n % 1024 == 0) ? 16 : 0;
	c->cl = c->page * c->ppc;
	c->alloc_offset = le32(h + 0x34);
	c->alloc_end = le32(h + 0x38);
	c->rootdir = le32(h + 0x3C);
	for (k = 0; k < 32; k++)
		c->ifc[k] = le32(h + 0x50 + 4 * k);

	/* the root: its first entry carries the directory's length */
	e0 = dir_entries(c, c->rootdir, 1);
	if (!e0)
		return -3;
	rootlen = le32(e0 + 4);
	free(e0);
	if (rootlen > 4096)
		rootlen = 4096;
	root = dir_entries(c, c->rootdir, rootlen);
	if (!root)
		return -3;

	/* free space: free clusters in the FAT, as the PS2 counts them */
	{
		u32 nfree = 0, total = c->alloc_end;

		for (k = 0; k < total; k++)
			if (!(fat_entry(c, k) & 0x80000000))
				nfree++;
		c->free_kb = (int)((u64)nfree * c->cl / 1024);
	}

	for (i = 0; i < rootlen && c->n < MC_SAVES; i++)
	{
		const u8 *e = root + 512 * i;
		u32 mode = le16(e), dlen = le32(e + 4), dfirst = le32(e + 0x10), j;
		char name[33];
		save_t *s;
		u8 *ents;
		u8 *sys = NULL;
		u32 syslen = 0, iconlen = 0;
		char iconfile[33];

		memcpy(name, e + 0x40, 32);
		name[32] = 0;
		if (!(mode & 0x8000) || !(mode & 0x0020) || !name[0] || name[0] == '.')
			continue;
		s = &c->saves[c->n];
		memset(s, 0, sizeof *s);
		strcpy(s->dir, name);
		strcpy(s->title, name);
		s->first = (int)dfirst;
		s->slot = (int)i;
		s->dirlen = (int)dlen;
		fmt_when(s->when, e + 0x18);
		if (dlen > 256)
			dlen = 256;
		ents = dir_entries(c, dfirst, dlen);
		iconfile[0] = 0;
		for (j = 0; ents && j < dlen; j++)
		{
			const u8 *f = ents + 512 * j;
			u32 fm = le16(f), fl = le32(f + 4);
			char fn[33];

			memcpy(fn, f + 0x40, 32);
			fn[32] = 0;
			if (!(fm & 0x8000) || (fm & 0x0020) || !(fm & 0x0010))
				continue;
			s->bytes += fl;
			s->nfiles++;
			if (!strcmp(fn, "icon.sys") && fl >= 964)
			{
				sys = chain(c, le32(f + 0x10), fl);
				syslen = fl;
			}
		}
		if (sys && syslen >= 964 && !memcmp(sys, "PS2D", 4))
		{
			int brk = (int)le16(sys + 6);

			sjis_ascii(s->title, sizeof s->title, sys + 0xC0, 68, brk);
			if (!has_letters(s->title))
				strcpy(s->title, s->dir);
			memcpy(iconfile, sys + 0x104, 32);
			iconfile[32] = 0;
		}
		free(sys);
		for (j = 0; ents && iconfile[0] && j < dlen; j++)
		{
			const u8 *f = ents + 512 * j;
			char fn[33];

			memcpy(fn, f + 0x40, 32);
			fn[32] = 0;
			if ((le16(f) & 0x8010) == 0x8010 && !(le16(f) & 0x20) && !strcmp(fn, iconfile))
			{
				iconlen = le32(f + 4);
				if (iconlen >= 64 && iconlen <= 0x80000)
				{
					u8 *ic = chain(c, le32(f + 0x10), iconlen);

					if (ic && ps2_icon(s, ic, iconlen) != 0)
					{
						free(s->v);
						free(s->px);
						s->v = NULL;
						s->px = NULL;
						s->icon = 0;
					}
					free(ic);
				}
			}
		}
		free(ents);
		c->n++;
	}
	free(root);
	return 0;
}

/* ------------------------------------------------------------------ PS1 cards */

static int load_ps1(card_t *c)
{
	int f;

	if (c->len != 131072 || c->raw[0] != 'M' || c->raw[1] != 'C')
		return -1;
	c->free_kb = 0;
	for (f = 1; f < 16; f++)
	{
		const u8 *d = c->raw + 128 * f;

		if ((d[0] & 0xF0) == 0xA0)
			c->free_kb += 8;
	}
	for (f = 1; f < 16 && c->n < MC_SAVES; f++)
	{
		const u8 *d = c->raw + 128 * f, *blk = c->raw + 8192 * f;
		save_t *s;
		int k, b, i;

		if (d[0] != 0x51)		/* the first block of a save */
			continue;
		s = &c->saves[c->n];
		memset(s, 0, sizeof *s);
		memcpy(s->dir, d + 0x0A, 20);
		s->dir[20] = 0;
		s->first = f;
		s->bytes = le32(d + 4);
		s->nfiles = 1;
		for (b = f, i = 0; b >= 1 && b < 16 && i < 15; i++)
		{
			u32 nx = le16(c->raw + 128 * b + 8);

			s->blocks++;
			if (nx == 0xFFFF)
				break;
			b = (int)nx + 1;
		}
		strcpy(s->title, s->dir);
		if (blk[0] == 'S' && blk[1] == 'C')
		{
			sjis_ascii(s->title, sizeof s->title, blk + 4, 64, -1);
			if (!has_letters(s->title))
				strcpy(s->title, s->dir);
			s->frames = (blk[2] & 0x0F) >= 1 && (blk[2] & 0x0F) <= 3 ? (blk[2] & 0x0F) : 1;
			s->px = (u8 *)malloc(16 * 16 * 4 * 3);
			if (s->px)
			{
				for (k = 0; k < s->frames; k++)
				{
					const u8 *bits = blk + 128 * (1 + k);

					for (i = 0; i < 256; i++)
					{
						u32 col = le16(blk + 0x60 + 2 * ((bits[i / 2] >> ((i & 1) * 4)) & 15));
						u8 *o = s->px + (k * 256 + i) * 4;

						PX16(col, o);
						o[3] = col ? 0x80 : 0;	/* colour 0000 is clear */
					}
				}
				s->tex.w = 16;
				s->tex.h = 16 * s->frames;
				s->tex.px = s->px;
				s->icon = 1;
			}
		}
		c->n++;
	}
	return 0;
}

/* ------------------------------------------------------------------ */

int vmc_load(card_t *c, const char *path)
{
	FILE *f;
	const char *b = strrchr(path, '/'), *dot;

	memset(c, 0, sizeof *c);
	snprintf(c->path, sizeof c->path, "%s", path);
	b = b ? b + 1 : path;
	snprintf(c->name, sizeof c->name, "%s", b);
	dot = strrchr(c->name, '.');
	c->ps1 = dot && (dot[3] == '1');
	if (dot)
		c->name[dot - c->name] = 0;
	f = fopen(path, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	c->len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (c->len <= 0 || c->len > 16 * 1024 * 1024)
	{
		fclose(f);
		return -2;
	}
	c->raw = (u8 *)malloc(c->len);
	if (!c->raw || fread(c->raw, 1, c->len, f) != (size_t)c->len)
	{
		fclose(f);
		return -3;
	}
	fclose(f);
	c->ok = (c->ps1 ? load_ps1(c) : load_ps2(c)) == 0;
	return c->ok ? 0 : -4;
}

void vmc_free(card_t *c)
{
	int i;

	for (i = 0; i < c->n; i++)
	{
		free(c->saves[i].v);
		free(c->saves[i].px);
		free(c->saves[i].still);	/* v0.27 */
	}
	free(c->raw);
	memset(c, 0, sizeof *c);
}

u8 *vmc_read_file(card_t *c, const save_t *s, const char *name, u32 *len)
{
	u8 *ents, *out = NULL;
	u32 dlen = (u32)s->dirlen, j;	/* the root entry's count: the folder's own "." holds 0 */

	if (c->ps1)
		return NULL;
	ents = dir_entries(c, (u32)s->first, dlen);
	for (j = 0; ents && j < dlen; j++)
	{
		const u8 *f = ents + 512 * j;

		if ((le16(f) & 0x8030) == 0x8010 && !strncmp((const char *)f + 0x40, name, 32))
		{
			*len = le32(f + 4);
			out = chain(c, le32(f + 0x10), *len);
			break;
		}
	}
	free(ents);
	return out;
}

#ifdef __PPU__
static int name_cmp_ci(const void *a, const void *b)
{
	return strcasecmp(((const card_t *)a)->name, ((const card_t *)b)->name);
}

int vmc_load_all(void)
{
	sysFSDirent e;
	u64 rd;
	s32 fd;
	int i;

	for (i = 0; i < n_cards; i++)
		vmc_free(&cards[i]);
	n_cards = 0;
	if (sysFsOpendir(VMC_DIR, &fd) != 0)
		return 0;
	while (sysFsReaddir(fd, &e, &rd) == 0 && rd > 0 && n_cards < MC_CARDS)
	{
		char path[200];
		size_t l = strlen(e.d_name);

		if (l < 5 || (strcasecmp(e.d_name + l - 4, ".VM2") && strcasecmp(e.d_name + l - 4, ".VM1")))
			continue;
		snprintf(path, sizeof path, "%s/%s", VMC_DIR, e.d_name);
		if (vmc_load(&cards[n_cards], path) == 0 || cards[n_cards].raw)
			n_cards++;
	}
	sysFsClosedir(fd);
	/* PS2 cards first, then PS1, A-Z within */
	qsort(cards, n_cards, sizeof(card_t), name_cmp_ci);
	return n_cards;
}
#endif
