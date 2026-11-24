/* Writing the PS3's virtual memory cards: copy a save to another card, delete a save.
 *
 * Everything here copies what the owner's real cards were measured to hold (host/vmc_inspect.py,
 * 2026-11-22), not what a document says:
 *  - every written page carries ECC in its spare bytes: 3 bytes per 128-byte chunk, then 4 zero
 *    bytes (6327 of 6327 written pages agree with vmc_ecc128); erased pages are all FF;
 *  - a PS2 deletes by clearing the "exists" bit (0x8000) of the entry and the allocated bit of
 *    each FAT entry, keeping the links (OPL.VM2 holds 1767 such freed links and a deleted ELF);
 *  - a save's "." entry: length 0, cluster 0, dirent = its index in the root; its ".." entry:
 *    mode 8427, length 0, cluster 0; the folder's entry COUNT lives in its root entry;
 *  - PS1 (.VM1, raw 128 KB): a save's directory frames go 51/52/53 -> A1/A2/A3 when deleted
 *    (psx-spx); each frame ends in the XOR of its first 127 bytes.
 *
 * The card's file is written only after the whole change is made in memory, only the pages that
 * changed, and only after the file as it was has been kept in vmc_bak_dir:
 *   <file>.orig   the card before Prism first changed it (never overwritten)
 *   <file>.last   the card before the latest change */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __PPU__
#include <lv2/sysfs.h>
#else
#include <sys/stat.h>
#endif
#include "vmc.h"

const char *vmc_bak_dir = "/dev_hdd0/tmp/prism_vmc_bak";

#define FAT_ALLOC	0x80000000u
#define FAT_END		0xFFFFFFFFu
#define DF_EXISTS	0x8000
#define DF_DIR		0x0020
#define DF_FILE		0x0010
#define MAX_CL		8192

static u32 le32(const u8 *p) { return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u32 le16(const u8 *p) { return p[0] | ((u32)p[1] << 8); }
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

/* ------------------------------------------------------------------ ECC */

static u32 par(u32 b)
{
	b ^= b >> 4;
	b ^= b >> 2;
	b ^= b >> 1;
	return b & 1;
}

void vmc_ecc128(const u8 *s, u8 out[3])
{
	u32 cp = 0x77, lp0 = 0x7F, lp1 = 0x7F;
	int i;

	for (i = 0; i < 128; i++)
	{
		u32 a = s[i];

		cp ^= par(a & 0x55) | (par(a & 0x33) << 1) | (par(a & 0x0F) << 2) | (par(a & 0xAA) << 4) | (par(a & 0xCC) << 5) | (par(a & 0xF0) << 6);
		if (par(a))
		{
			lp0 ^= ~(u32)i & 0x7F;
			lp1 ^= (u32)i;
		}
	}
	out[0] = (u8)cp;
	out[1] = (u8)(lp0 & 0x7F);
	out[2] = (u8)(lp1 & 0x7F);
}

/* ------------------------------------------------------------------ pages and clusters */

typedef struct
{
	card_t *c;
	u8 *dirty;		/* one byte per page */
	u32 npages;
	int bad;		/* an address fell outside the card: the change is abandoned */
} wr_t;

static u8 *page(card_t *c, u32 p)
{
	return c->raw + (long)p * (c->page + c->spare);
}

static void cl_read(card_t *c, u32 abscl, u32 off, u8 *dst, u32 n)
{
	while (n)
	{
		u32 p = abscl * c->ppc + off / c->page, o = off % c->page, k = c->page - o;

		if (k > n)
			k = n;
		if ((long)(p + 1) * (c->page + c->spare) > c->len)
			memset(dst, 0xFF, k);
		else
			memcpy(dst, page(c, p) + o, k);
		dst += k;
		off += k;
		n -= k;
	}
}

static void cl_write(wr_t *w, u32 abscl, u32 off, const u8 *src, u32 n)
{
	card_t *c = w->c;

	while (n)
	{
		u32 p = abscl * c->ppc + off / c->page, o = off % c->page, k = c->page - o;

		if (k > n)
			k = n;
		if (p >= w->npages)
		{
			w->bad = 1;
			return;
		}
		memcpy(page(c, p) + o, src, k);
		w->dirty[p] = 1;
		src += k;
		off += k;
		n -= k;
	}
}

static int fat_where(card_t *c, u32 n, u32 *fc)
{
	u32 per = c->cl / 4;
	u8 b[4];

	if (n >= c->alloc_end || n / (per * per) >= 32)
		return -1;
	cl_read(c, c->ifc[n / (per * per)], 4 * ((n / per) % per), b, 4);
	*fc = le32(b);
	return 0;
}

static u32 fat_get(card_t *c, u32 n)
{
	u32 fc;
	u8 b[4];

	if (fat_where(c, n, &fc))
		return FAT_END;
	cl_read(c, fc, 4 * (n % (c->cl / 4)), b, 4);
	return le32(b);
}

static void fat_set(wr_t *w, u32 n, u32 v)
{
	u32 fc;
	u8 b[4];

	if (fat_where(w->c, n, &fc))
	{
		w->bad = 1;
		return;
	}
	put32(b, v);
	cl_write(w, fc, 4 * (n % (w->c->cl / 4)), b, 4);
}

/* the clusters (relative) of a chain, at most max; returns the count */
static u32 chain_list(card_t *c, u32 first, u32 max, u32 *out)
{
	u32 n = 0, k = first;

	while (n < max && k < c->alloc_end)
	{
		u32 e = fat_get(c, k);

		out[n++] = k;
		if (e == FAT_END || !(e & FAT_ALLOC))
			break;
		k = e & ~FAT_ALLOC;
	}
	return n;
}

/* the first `need` free clusters, lowest first, as the PS2 picks them */
static u32 free_list(card_t *c, u32 need, u32 *out)
{
	u32 n = 0, k;

	for (k = 0; k < c->alloc_end && n < need; k++)
		if (!(fat_get(c, k) & FAT_ALLOC))
			out[n++] = k;
	return n;
}

/* link a list of clusters into a chain */
static void link_chain(wr_t *w, const u32 *cl, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		fat_set(w, cl[i], i + 1 < n ? (cl[i + 1] | FAT_ALLOC) : FAT_END);
}

/* free a chain the way the PS2 does: clear the allocated bit, keep the link */
static void free_chain(wr_t *w, u32 first)
{
	u32 k = first, guard = 0;

	while (k < w->c->alloc_end && guard++ < MAX_CL)
	{
		u32 e = fat_get(w->c, k);

		if (!(e & FAT_ALLOC))
			break;
		fat_set(w, k, e & ~FAT_ALLOC);
		if (e == FAT_END)
			break;
		k = e & ~FAT_ALLOC;
	}
}

/* entry i of a directory whose clusters are listed in cl[] */
static void ent_read(card_t *c, const u32 *cl, u32 i, u8 *e)
{
	u32 per = c->cl / 512;

	cl_read(c, c->alloc_offset + cl[i / per], (i % per) * 512, e, 512);
}

static void ent_write(wr_t *w, const u32 *cl, u32 i, const u8 *e)
{
	u32 per = w->c->cl / 512;

	cl_write(w, w->c->alloc_offset + cl[i / per], (i % per) * 512, e, 512);
}

/* ------------------------------------------------------------------ the file */

static void make_dir(const char *p)
{
#ifdef __PPU__
	sysFsMkdir(p, 0777);
#else
	mkdir(p, 0777);
#endif
}

static int write_whole(const char *path, const u8 *d, long n)
{
	FILE *f = fopen(path, "wb");
	int ok;

	if (!f)
		return -1;
	ok = fwrite(d, 1, n, f) == (size_t)n;
	if (fclose(f) != 0)
		ok = 0;
	return ok ? 0 : -1;
}

/* keep the card as it is now; called BEFORE anything in c->raw changes */
static int keep(card_t *c)
{
	char p[300];
	const char *b = strrchr(c->path, '/');
	FILE *f;

	b = b ? b + 1 : c->path;
	make_dir(vmc_bak_dir);
	snprintf(p, sizeof p, "%s/%s.orig", vmc_bak_dir, b);
	f = fopen(p, "rb");
	if (f)
		fclose(f);
	else if (write_whole(p, c->raw, c->len))
		return -1;
	snprintf(p, sizeof p, "%s/%s.last", vmc_bak_dir, b);
	return write_whole(p, c->raw, c->len);
}

/* the changed pages, each with its ECC, into the card's file */
static int flush(wr_t *w)
{
	card_t *c = w->c;
	long step = c->page + c->spare;
	FILE *f;
	u32 p;
	int ok = 1;

	f = fopen(c->path, "r+b");
	if (!f)
		return -1;
	for (p = 0; p < w->npages && ok; p++)
	{
		u8 *d;
		int k;

		if (!w->dirty[p])
			continue;
		d = page(c, p);
		if (c->spare >= 12)
		{
			for (k = 0; k < 4; k++)
				vmc_ecc128(d + 128 * k, d + c->page + 3 * k);
			memset(d + c->page + 12, 0, c->spare - 12);
		}
		ok = fseek(f, (long)p * step, SEEK_SET) == 0 && fwrite(d, 1, step, f) == (size_t)step;
	}
	if (fclose(f) != 0)
		ok = 0;
	return ok ? 0 : -1;
}

static int wr_open(wr_t *w, card_t *c)
{
	memset(w, 0, sizeof *w);
	w->c = c;
	w->npages = (u32)(c->len / (c->page + c->spare));
	w->dirty = (u8 *)calloc(w->npages, 1);
	return w->dirty ? 0 : -1;
}

/* ------------------------------------------------------------------ PS2 */

static u32 cl_count(card_t *c, u32 bytes)
{
	return (bytes + c->cl - 1) / c->cl;
}

/* the root directory's clusters and entry count */
static u32 root_list(card_t *c, u32 *cl, u32 *rlen)
{
	u8 e[512];
	u32 n = chain_list(c, c->rootdir, 1, cl);

	if (!n)
		return 0;
	ent_read(c, cl, 0, e);
	*rlen = le32(e + 4);
	if (*rlen < 2 || *rlen > 4096)
		return 0;
	return chain_list(c, c->rootdir, cl_count(c, *rlen * 512), cl);
}

static int copy_ps2(card_t *from, save_t *s, card_t *to, char *why, int why_n)
{
	static u32 rcl[MAX_CL], scl[MAX_CL], dcl[MAX_CL], fcl[MAX_CL], newcl[MAX_CL];
	static u8 ents[256][512];
	u32 dl = (u32)s->dirlen, nd, j, nf = 0, need = 0, rlen, nr, slot, i, used = 0;
	u32 tr_rlen;
	u8 root_e[512], e[512];
	int append = 0;
	wr_t w;

	if (dl < 2 || dl > 256)
	{
		snprintf(why, why_n, "the save's folder could not be read");
		return -1;
	}
	/* the source: its folder and its files */
	nd = chain_list(from, (u32)s->first, cl_count(from, dl * 512), scl);
	if (nd < cl_count(from, dl * 512))
	{
		snprintf(why, why_n, "the save's folder is broken on this card");
		return -1;
	}
	for (j = 0; j < dl; j++)
	{
		u8 *f = ents[nf];

		ent_read(from, scl, j, f);
		if (j < 2)
		{
			nf++;
			continue;
		}
		if ((le16(f) & (DF_EXISTS | DF_DIR | DF_FILE)) != (DF_EXISTS | DF_FILE))
			continue;	/* deleted entries and sub-folders stay behind */
		need += cl_count(from, le32(f + 4));
		nf++;
	}
	nf -= 2;
	nr = root_list(from, rcl, &rlen);
	if (!nr || (u32)s->slot >= rlen)
	{
		snprintf(why, why_n, "the card's root could not be read");
		return -1;
	}
	ent_read(from, rcl, (u32)s->slot, root_e);

	/* the target: same name already there? a free root slot? */
	nr = root_list(to, rcl, &tr_rlen);
	if (!nr)
	{
		snprintf(why, why_n, "the other card's root could not be read");
		return -1;
	}
	slot = 0;
	for (i = 2; i < tr_rlen; i++)
	{
		ent_read(to, rcl, i, e);
		if (le16(e) & DF_EXISTS)
		{
			if (!strncmp((const char *)e + 0x40, (const char *)root_e + 0x40, 32))
			{
				snprintf(why, why_n, "that card already has this save");
				return -1;
			}
		}
		else if (!slot)
			slot = i;
	}
	if (!slot)
	{
		slot = tr_rlen;
		append = 1;
		if ((tr_rlen * 512) % to->cl == 0)
			need++;		/* the root grows by a cluster */
	}
	need += cl_count(to, (2 + nf) * 512);
	if (need > MAX_CL || free_list(to, need, newcl) < need)
	{
		snprintf(why, why_n, "not enough room on that card (%u KB needed)", need * to->cl / 1024);
		return -1;
	}
	if (keep(to))
	{
		snprintf(why, why_n, "could not keep a backup of that card first");
		return -1;
	}
	if (wr_open(&w, to))
	{
		snprintf(why, why_n, "out of memory");
		return -1;
	}

	/* the files: cluster for cluster as they are, then their new first cluster in the entry */
	for (j = 2; j < 2 + nf; j++)
	{
		u8 *f = ents[j], buf[1024];
		u32 len = le32(f + 4), n = cl_count(from, len), got, k;

		if (!n)
		{
			put32(f + 0x10, FAT_END);
			continue;
		}
		got = chain_list(from, le32(f + 0x10), n, fcl);
		if (got < n || to->cl > sizeof buf)
		{
			free(w.dirty);
			snprintf(why, why_n, "a file of the save is broken on this card");
			return -1;
		}
		for (k = 0; k < n; k++)
		{
			cl_read(from, from->alloc_offset + fcl[k], 0, buf, from->cl);
			cl_write(&w, to->alloc_offset + newcl[used + k], 0, buf, to->cl);
		}
		link_chain(&w, newcl + used, n);
		put32(f + 0x10, newcl[used]);
		used += n;
	}

	/* the folder: ".", "..", then the files that exist, nothing else */
	{
		u32 n = cl_count(to, (2 + nf) * 512), k;
		u8 z[512];

		memcpy(dcl, newcl + used, n * sizeof(u32));
		used += n;
		put32(ents[0] + 0x14, slot);
		memset(z, 0, sizeof z);
		for (k = 0; k < n * (to->cl / 512); k++)
			ent_write(&w, dcl, k, k < 2 + nf ? ents[k] : z);
		link_chain(&w, dcl, n);
	}

	/* the root: the save's entry, pointing at its new folder */
	put32(root_e + 4, 2 + nf);
	put32(root_e + 0x10, dcl[0]);
	put32(root_e + 0x14, 0);
	if (append)
	{
		if ((tr_rlen * 512) % to->cl == 0)
		{
			u8 z[1024];

			memset(z, 0, sizeof z);
			rcl[nr] = newcl[used++];
			cl_write(&w, to->alloc_offset + rcl[nr], 0, z, to->cl);
			fat_set(&w, rcl[nr - 1], rcl[nr] | FAT_ALLOC);
			fat_set(&w, rcl[nr], FAT_END);
			nr++;
		}
		ent_read(to, rcl, 0, e);
		put32(e + 4, tr_rlen + 1);
		ent_write(&w, rcl, 0, e);
	}
	ent_write(&w, rcl, slot, root_e);

	if (w.bad || flush(&w))
	{
		free(w.dirty);
		snprintf(why, why_n, w.bad ? "the card's layout is not what it should be" : "could not write the card");
		return -1;
	}
	free(w.dirty);
	return 0;
}

static int delete_ps2(card_t *c, save_t *s, char *why, int why_n)
{
	static u32 rcl[MAX_CL], dcl[MAX_CL];
	u32 dl = (u32)s->dirlen, nd, j, rlen;
	u8 e[512];
	wr_t w;

	if (dl < 2 || dl > 256 || !root_list(c, rcl, &rlen) || (u32)s->slot >= rlen)
	{
		snprintf(why, why_n, "the save could not be read");
		return -1;
	}
	nd = chain_list(c, (u32)s->first, cl_count(c, dl * 512), dcl);
	if (nd < cl_count(c, dl * 512))
	{
		snprintf(why, why_n, "the save's folder is broken");
		return -1;
	}
	if (keep(c))
	{
		snprintf(why, why_n, "could not keep a backup of the card first");
		return -1;
	}
	if (wr_open(&w, c))
	{
		snprintf(why, why_n, "out of memory");
		return -1;
	}
	for (j = 2; j < dl; j++)
	{
		ent_read(c, dcl, j, e);
		if ((le16(e) & (DF_EXISTS | DF_DIR | DF_FILE)) != (DF_EXISTS | DF_FILE))
			continue;
		if (le32(e + 4))
			free_chain(&w, le32(e + 0x10));
		put16(e, le16(e) & ~DF_EXISTS);
		ent_write(&w, dcl, j, e);
	}
	free_chain(&w, (u32)s->first);
	ent_read(c, rcl, (u32)s->slot, e);
	put16(e, le16(e) & ~DF_EXISTS);
	ent_write(&w, rcl, (u32)s->slot, e);
	if (w.bad || flush(&w))
	{
		free(w.dirty);
		snprintf(why, why_n, w.bad ? "the card's layout is not what it should be" : "could not write the card");
		return -1;
	}
	free(w.dirty);
	return 0;
}

/* ------------------------------------------------------------------ PS1 */

static void ps1_sum(u8 *f)
{
	u8 x = 0;
	int i;

	for (i = 0; i < 127; i++)
		x ^= f[i];
	f[127] = x;
}

static int ps1_chain(card_t *c, int first, int *out)
{
	int n = 0, b = first;

	while (b >= 1 && b < 16 && n < 15)
	{
		u32 nx = le16(c->raw + 128 * b + 8);

		out[n++] = b;
		if (nx == 0xFFFF)
			break;
		b = (int)nx + 1;
	}
	return n;
}

static int delete_ps1(card_t *c, save_t *s, char *why, int why_n)
{
	int bl[15], n = ps1_chain(c, s->first, bl), k;

	if (!n)
	{
		snprintf(why, why_n, "the save could not be read");
		return -1;
	}
	if (keep(c))
	{
		snprintf(why, why_n, "could not keep a backup of the card first");
		return -1;
	}
	for (k = 0; k < n; k++)
	{
		u8 *f = c->raw + 128 * bl[k];

		if (f[0] >= 0x51 && f[0] <= 0x53)
			f[0] = (u8)(f[0] + 0x50);	/* 51/52/53 -> A1/A2/A3 */
		ps1_sum(f);
	}
	if (write_whole(c->path, c->raw, c->len))
	{
		snprintf(why, why_n, "could not write the card");
		return -1;
	}
	return 0;
}

static int copy_ps1(card_t *from, save_t *s, card_t *to, char *why, int why_n)
{
	int bl[15], fr[15], n = ps1_chain(from, s->first, bl), nfree = 0, k, b;

	if (!n)
	{
		snprintf(why, why_n, "the save could not be read");
		return -1;
	}
	for (b = 1; b < 16; b++)
	{
		const u8 *f = to->raw + 128 * b;

		if (f[0] == 0x51 && !memcmp(f + 0x0A, from->raw + 128 * bl[0] + 0x0A, 20))
		{
			snprintf(why, why_n, "that card already has this save");
			return -1;
		}
		if ((f[0] & 0xF0) == 0xA0 && nfree < 15)
			fr[nfree++] = b;
	}
	if (nfree < n)
	{
		snprintf(why, why_n, "not enough room on that card (%d blocks needed)", n);
		return -1;
	}
	if (keep(to))
	{
		snprintf(why, why_n, "could not keep a backup of that card first");
		return -1;
	}
	for (k = 0; k < n; k++)
	{
		u8 *f = to->raw + 128 * fr[k];

		memcpy(f, from->raw + 128 * bl[k], 128);
		put16(f + 8, k + 1 < n ? (u32)(fr[k + 1] - 1) : 0xFFFF);
		ps1_sum(f);
		memcpy(to->raw + 8192 * fr[k], from->raw + 8192 * bl[k], 8192);
	}
	if (write_whole(to->path, to->raw, to->len))
	{
		snprintf(why, why_n, "could not write the card");
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */

int vmc_copy(card_t *from, int si, card_t *to, char *why, int why_n)
{
	if (!from->ok || !to->ok || si < 0 || si >= from->n || from == to)
	{
		snprintf(why, why_n, "nothing to copy");
		return -1;
	}
	if (from->ps1 != to->ps1)
	{
		snprintf(why, why_n, "PS1 and PS2 cards do not mix");
		return -1;
	}
	return from->ps1 ? copy_ps1(from, &from->saves[si], to, why, why_n) : copy_ps2(from, &from->saves[si], to, why, why_n);
}

int vmc_delete(card_t *c, int si, char *why, int why_n)
{
	if (!c->ok || si < 0 || si >= c->n)
	{
		snprintf(why, why_n, "nothing to delete");
		return -1;
	}
	return c->ps1 ? delete_ps1(c, &c->saves[si], why, why_n) : delete_ps2(c, &c->saves[si], why, why_n);
}
