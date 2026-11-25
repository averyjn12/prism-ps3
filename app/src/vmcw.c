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

/* v0.67: a save on its way from one place to another: out of a card (pkg_from_card) or a .psu file
 * (vmc_import_psu), into a card (pkg_to_card) or a .psu file (vmc_export_psu). Its root entry, its
 * folder's entries, and each file's bytes in whole 1024-byte clusters, slack and all: a save copied
 * card to card through this is byte for byte what v0.8-v0.66 wrote (host/vmcw_regress.sh). */
#define PKG_FILES	254
#define PKG_CL		1024

typedef struct
{
	u8 root_e[512];
	u8 ents[2 + PKG_FILES][512];	/* 0 ".", 1 "..", then the files that exist */
	u32 nf;
	u8 *data[PKG_FILES];		/* ncl[j] x 1024 bytes; NULL for an empty file */
	u32 ncl[PKG_FILES];
} pkg_t;

static pkg_t pk;	/* one at a time: 130 KB, too big for the stack */

static void pkg_free(pkg_t *p)
{
	u32 j;

	for (j = 0; j < PKG_FILES; j++)
	{
		free(p->data[j]);
		p->data[j] = NULL;
		p->ncl[j] = 0;
	}
	p->nf = 0;
}

static int pkg_from_card(card_t *from, save_t *s, pkg_t *p, char *why, int why_n)
{
	static u32 rcl[MAX_CL], scl[MAX_CL], fcl[MAX_CL];
	u32 dl = (u32)s->dirlen, nd, j, nf = 0, rlen, nr;

	pkg_free(p);
	if (dl < 2 || dl > 256)
	{
		snprintf(why, why_n, "the save's folder could not be read");
		return -1;
	}
	if (from->cl != PKG_CL)
	{
		snprintf(why, why_n, "this card's cluster size is not 1024 bytes");
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
		u8 *f = p->ents[nf];

		ent_read(from, scl, j, f);
		if (j < 2)
		{
			nf++;
			continue;
		}
		if ((le16(f) & (DF_EXISTS | DF_DIR | DF_FILE)) != (DF_EXISTS | DF_FILE))
			continue;	/* deleted entries and sub-folders stay behind */
		nf++;
	}
	nf -= 2;
	nr = root_list(from, rcl, &rlen);
	if (!nr || (u32)s->slot >= rlen)
	{
		snprintf(why, why_n, "the card's root could not be read");
		return -1;
	}
	ent_read(from, rcl, (u32)s->slot, p->root_e);
	/* the files: cluster for cluster as they are */
	for (j = 0; j < nf; j++)
	{
		u8 *f = p->ents[2 + j];
		u32 len = le32(f + 4), n = cl_count(from, len), got, k;

		if (!n)
			continue;
		got = chain_list(from, le32(f + 0x10), n, fcl);
		p->data[j] = got < n ? NULL : (u8 *)malloc((size_t)n * PKG_CL);
		if (!p->data[j])
		{
			snprintf(why, why_n, got < n ? "a file of the save is broken on this card" : "out of memory");
			pkg_free(p);
			return -1;
		}
		p->ncl[j] = n;
		for (k = 0; k < n; k++)
			cl_read(from, from->alloc_offset + fcl[k], 0, p->data[j] + (size_t)k * PKG_CL, PKG_CL);
	}
	p->nf = nf;
	return 0;
}

static int pkg_to_card(pkg_t *p, card_t *to, char *why, int why_n)
{
	static u32 rcl[MAX_CL], dcl[MAX_CL], newcl[MAX_CL];
	u32 nf = p->nf, j, need = 0, nr, slot, i, used = 0;
	u32 tr_rlen;
	u8 *root_e = p->root_e, e[512];
	int append = 0;
	wr_t w;

	if (to->cl != PKG_CL)
	{
		snprintf(why, why_n, "that card's cluster size is not 1024 bytes");
		return -1;
	}
	for (j = 0; j < nf; j++)
		need += p->ncl[j];
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
	for (j = 0; j < nf; j++)
	{
		u8 *f = p->ents[2 + j];
		u32 n = p->ncl[j], k;

		if (!n)
		{
			put32(f + 0x10, FAT_END);
			continue;
		}
		for (k = 0; k < n; k++)
			cl_write(&w, to->alloc_offset + newcl[used + k], 0, p->data[j] + (size_t)k * PKG_CL, to->cl);
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
		put32(p->ents[0] + 0x14, slot);
		memset(z, 0, sizeof z);
		for (k = 0; k < n * (to->cl / 512); k++)
			ent_write(&w, dcl, k, k < 2 + nf ? p->ents[k] : z);
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

static int copy_ps2(card_t *from, save_t *s, card_t *to, char *why, int why_n)
{
	int r = pkg_from_card(from, s, &pk, why, why_n);

	if (r == 0)
		r = pkg_to_card(&pk, to, why, why_n);
	pkg_free(&pk);
	return r;
}

/* ---- the .psu file: a save as other PS2 tools pass it around (uLaunchELF's "psuPaste", mymc, PCSX2's
 * importers). It is the card's own 512-byte directory entries in a row: the save's folder (its length =
 * the number of entries that follow), ".", "..", then each file's entry followed by the file's bytes
 * filled up to a multiple of 1024. The cluster and place fields mean nothing in a file and are written
 * as 0. Judged by mymcplus, which reads what this writes and writes what this reads (host/psu_test.sh). */

static int name_ok(const u8 *e)
{
	const char *n = (const char *)e + 0x40;
	int k;

	for (k = 0; k < 32 && n[k]; k++)
		if ((unsigned char)n[k] < 0x20 || n[k] == '/' || n[k] == '\\' || n[k] == '?' || n[k] == '*' || n[k] == ':')
			return 0;
	return k > 0 && k < 32 && strcmp(n, ".") && strcmp(n, "..");
}

int vmc_export_psu(card_t *c, int si, const char *path, char *why, int why_n)
{
	u8 e[512];
	FILE *f;
	u32 j;
	int ok = 1;

	why[0] = 0;
	if (si < 0 || si >= c->n || !c->ok || c->ps1)
	{
		snprintf(why, why_n, "only a PS2 save can be written as a .psu file");
		return -1;
	}
	if (pkg_from_card(c, &c->saves[si], &pk, why, why_n))
		return -1;
	f = fopen(path, "wb");
	if (!f)
	{
		pkg_free(&pk);
		snprintf(why, why_n, "the file could not be made there");
		return -1;
	}
	memcpy(e, pk.root_e, 512);
	put32(e + 4, 2 + pk.nf);
	put32(e + 0x10, 0);
	put32(e + 0x14, 0);
	ok = fwrite(e, 1, 512, f) == 512;
	for (j = 0; j < 2 + pk.nf && ok; j++)
	{
		memcpy(e, pk.ents[j], 512);
		put32(e + 0x10, 0);
		put32(e + 0x14, 0);
		ok = fwrite(e, 1, 512, f) == 512;
		if (ok && j >= 2 && pk.ncl[j - 2])
			ok = fwrite(pk.data[j - 2], PKG_CL, pk.ncl[j - 2], f) == pk.ncl[j - 2];
	}
	if (fclose(f) != 0)
		ok = 0;
	pkg_free(&pk);
	if (!ok)
	{
		remove(path);
		snprintf(why, why_n, "the file could not be written whole (is there room?)");
		return -1;
	}
	return 0;
}

/* the save's folder name in a .psu file, without reading the rest (the screen lists files by it);
 * 0 and name[] filled (33 bytes) when the file starts as a .psu does */
int vmc_psu_name(const char *path, char *name, unsigned *files)
{
	u8 e[512];
	FILE *f = fopen(path, "rb");
	int ok;

	if (!f)
		return -1;
	ok = fread(e, 1, 512, f) == 512;
	fclose(f);
	if (!ok || (le16(e) & (DF_EXISTS | DF_DIR)) != (DF_EXISTS | DF_DIR) || le32(e + 4) < 2 || le32(e + 4) > 2 + PKG_FILES || !name_ok(e))
		return -1;
	memcpy(name, e + 0x40, 32);
	name[32] = 0;
	if (files)
		*files = le32(e + 4) - 2;
	return 0;
}

int vmc_import_psu(const char *path, card_t *to, char *why, int why_n)
{
	u8 *d = NULL;
	long len, at;
	FILE *f;
	u32 n, j, k;
	int r = -1;

	why[0] = 0;
	if (!to->ok || to->ps1)
	{
		snprintf(why, why_n, "a .psu file holds a PS2 save: choose a PS2 card");
		return -1;
	}
	f = fopen(path, "rb");
	if (!f)
	{
		snprintf(why, why_n, "the file could not be opened");
		return -1;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len < 3 * 512 || len > 9L * 1024 * 1024)
	{
		fclose(f);
		snprintf(why, why_n, "not a .psu save file (its size is wrong)");
		return -1;
	}
	d = (u8 *)malloc((size_t)len);
	if (!d || fread(d, 1, (size_t)len, f) != (size_t)len)
	{
		fclose(f);
		free(d);
		snprintf(why, why_n, "the file could not be read");
		return -1;
	}
	fclose(f);
	pkg_free(&pk);
	n = le32(d + 4);
	if ((le16(d) & (DF_EXISTS | DF_DIR)) != (DF_EXISTS | DF_DIR) || n < 2 || n > 2 + PKG_FILES || !name_ok(d))
	{
		snprintf(why, why_n, "not a .psu save file (its first entry is not a save's folder)");
		goto out;
	}
	memcpy(pk.root_e, d, 512);
	put16(pk.root_e, 0x8427);
	at = 512;
	for (j = 0; j < n; j++)
	{
		const u8 *e = d + at;
		u8 *o;
		u32 flen, cl;

		if (at + 512 > len)
		{
			snprintf(why, why_n, "the file ends before its %u entries do", n);
			goto out;
		}
		at += 512;
		if (j < 2)
		{
			/* "." and "..": as a PS2 writes them on a card (see the head of this file) */
			o = pk.ents[j];
			memcpy(o, e, 512);
			put16(o, 0x8427);
			put32(o + 4, 0);
			put32(o + 0x10, 0);
			put32(o + 0x14, 0);
			memset(o + 0x40, 0, 32);
			strcpy((char *)o + 0x40, j ? ".." : ".");
			continue;
		}
		if ((le16(e) & (DF_EXISTS | DF_DIR | DF_FILE)) != (DF_EXISTS | DF_FILE) || !name_ok(e))
		{
			snprintf(why, why_n, "entry %u of the file is not a plain file (a folder inside a save cannot be brought in)", j + 1);
			goto out;
		}
		for (k = 0; k < pk.nf; k++)
			if (!strncmp((const char *)pk.ents[2 + k] + 0x40, (const char *)e + 0x40, 32))
			{
				snprintf(why, why_n, "the file names \"%.31s\" twice", (const char *)e + 0x40);
				goto out;
			}
		flen = le32(e + 4);
		cl = (flen + PKG_CL - 1) / PKG_CL;
		if (flen > (u32)(len - at))
		{
			snprintf(why, why_n, "the file ends inside \"%.31s\"", (const char *)e + 0x40);
			goto out;
		}
		o = pk.ents[2 + pk.nf];
		memcpy(o, e, 512);
		put32(o + 0x10, 0);
		put32(o + 0x14, 0);
		if (cl)
		{
			u32 have = (u32)(len - at) < cl * PKG_CL ? (u32)(len - at) : cl * PKG_CL;	/* the last file's filling may be cut short */

			pk.data[pk.nf] = (u8 *)calloc(cl, PKG_CL);
			if (!pk.data[pk.nf])
			{
				snprintf(why, why_n, "out of memory");
				goto out;
			}
			memcpy(pk.data[pk.nf], d + at, have);
			pk.ncl[pk.nf] = cl;
			at += have;
		}
		pk.nf++;
	}
	r = pkg_to_card(&pk, to, why, why_n);
out:
	free(d);
	pkg_free(&pk);
	return r;
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
