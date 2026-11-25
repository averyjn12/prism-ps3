/* v0.37: adding one file to the SD card's exFAT volume (Bananafuzzbuddy, 11-23: "make it so we can rip to
 * exfat"; the console reads exFAT only through Prism's own reader, lr_fs.c, which never writes).
 *
 * What it does, per the exFAT specification (Microsoft, "exFAT file system specification"):
 *   - finds the exFAT volume (sector 0, or the first exFAT MBR primary partition) and checks it is
 *     one this code can write safely: 512-byte sectors, one FAT, not marked dirty;
 *   - reads the root folder for the allocation bitmap (entry 0x81) and up-case table (0x82), and
 *     finds the target folder (one level under the root);
 *   - takes ONE contiguous run of free clusters for the whole file (so the file needs no FAT chain:
 *     its stream entry says NoFatChain, as Windows writes contiguous files);
 *   - exw_write puts the data straight into those clusters;
 *   - exw_finish then marks the clusters in the bitmap, and last writes the folder entry set
 *     (File 0x85 + Stream Extension 0xC0 + File Name 0xC1 entries, with its SetChecksum and the
 *     name's NameHash through the volume's up-case table) into free slots of the target folder.
 * Order: data, bitmap, entry. Stopped before the bitmap, nothing changed on the volume; between the
 * bitmap and the entry, only clusters marked used that nothing names (what a disk check frees).
 * Every write is checked to lie inside the exFAT volume, so nothing outside it (the card's FAT32
 * partition, the MBR) can be touched. No FAT entries, no boot sectors, no other entries are
 * written.
 * v0.73: exw_delete, at the end of this file, takes one file out again. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "exfat_w.h"

#define SEC	512

static exw_dev D;
static uint32_t v_lba, v_len;		/* the volume: first sector, sectors */
static uint32_t fat_lba, heap_lba, nclus, root_clus;
static uint32_t spc_shift;		/* log2(sectors per cluster) */
static uint32_t bmp_clus;		/* allocation bitmap's first cluster */
static uint64_t bmp_len;
static uint8_t *bmp;			/* the whole bitmap */
static uint16_t *upcase;		/* 65536 entries */
static uint32_t dir_clus;		/* target folder */
static uint64_t dir_len;
static int dir_nofat;
static uint16_t uname[256];
static int ulen;
static uint64_t f_size;
static uint32_t f_clus, f_n;		/* the run: first cluster, clusters */
static uint64_t f_done;
static int active;
static uint8_t sbuf[SEC * 2];

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static void say(char *why, int wn, const char *s)
{
	if (why && wn > 0)
	{
		strncpy(why, s, (size_t)wn - 1);
		why[wn - 1] = 0;
	}
}

static int rd(uint32_t lba, uint32_t n, void *b)
{
	return D.rd(D.ctx, lba, n, b);
}

/* every write goes through here: inside the volume, or refused */
static int wr(uint32_t lba, uint32_t n, const void *b)
{
	if (lba < v_lba || n > v_len || lba - v_lba > v_len - n)
		return -100;
	return D.wr(D.ctx, lba, n, b);
}

static uint32_t clus_lba(uint32_t c)
{
	return heap_lba + ((c - 2) << spc_shift);
}

static uint8_t fb[SEC];		/* one FAT sector, cached; dropped by exw_begin */
static uint32_t fb_lba;

static uint32_t fat_next(uint32_t c)
{
	uint32_t s = fat_lba + (c * 4) / SEC;

	if (s != fb_lba || !fb_lba)
	{
		if (rd(s, 1, fb))
			return 0xFFFFFFFF;
		fb_lba = s;
	}
	return le32(fb + (c * 4) % SEC);
}

/* byte offset off of a cluster chain (FAT chain, or contiguous) -> sector; 0 = past its end */
static uint32_t chain_lba(uint32_t first, int nofat, uint64_t off)
{
	uint64_t csz = (uint64_t)SEC << spc_shift;
	uint32_t c = first, k = (uint32_t)(off / csz);

	if (nofat)
		c = first + k;
	else
		while (k--)
		{
			c = fat_next(c);
			if (c < 2 || c >= nclus + 2)
				return 0;
		}
	if (c < 2 || c >= nclus + 2)
		return 0;
	return clus_lba(c) + (uint32_t)((off % csz) / SEC);
}

static int find_volume(char *why, int wn)
{
	uint8_t b[SEC];
	uint32_t starts[5], lens[5], n = 0, i;

	if (rd(0, 1, b))
	{
		say(why, wn, "could not read the card");
		return -1;
	}
	starts[n] = 0;
	lens[n++] = D.sectors;
	if (b[510] == 0x55 && b[511] == 0xAA)
		for (i = 0; i < 4; i++)
		{
			const uint8_t *e = b + 446 + 16 * i;

			if (e[4] && le32(e + 8) && le32(e + 12))
			{
				starts[n] = le32(e + 8);
				lens[n++] = le32(e + 12);
			}
		}
	for (i = 0; i < n; i++)
	{
		if (starts[i] >= D.sectors || rd(starts[i], 1, b) || memcmp(b + 3, "EXFAT   ", 8))
			continue;
		if (b[108] != 9)
		{
			say(why, wn, "the exFAT volume does not use 512-byte sectors");
			return -1;
		}
		if (b[109] > 16 || b[110] != 1)
		{
			say(why, wn, "the exFAT volume has a layout this writer does not handle (cluster size or FAT count)");
			return -1;
		}
		if (le16(b + 106) & 0x0002)
		{
			say(why, wn, "the exFAT volume is marked as not cleanly closed: check it on a PC first");
			return -1;
		}
		v_lba = starts[i];
		v_len = (uint32_t)le64(b + 72);
		if (!v_len || v_len > lens[i] || v_lba + v_len > D.sectors || v_lba + v_len < v_lba)
			v_len = lens[i];
		fat_lba = v_lba + le32(b + 80);
		heap_lba = v_lba + le32(b + 88);
		nclus = le32(b + 92);
		root_clus = le32(b + 96);
		spc_shift = b[109];
		if (root_clus < 2 || root_clus >= nclus + 2 || heap_lba + ((uint64_t)nclus << spc_shift) > (uint64_t)v_lba + v_len)
		{
			say(why, wn, "the exFAT volume's numbers do not add up");
			return -1;
		}
		return 0;
	}
	say(why, wn, "no exFAT volume on the card");
	return -1;
}

/* walk a folder's 32-byte entries; cb returns 1 to stop. 0 = walked */
typedef int (*ent_cb)(const uint8_t *e, uint64_t off, void *arg);

static int walk(uint32_t first, int nofat, uint64_t len, ent_cb cb, void *arg)
{
	uint64_t off;
	uint32_t cur = 0;
	uint8_t b[SEC];

	for (off = 0; off < len; off += 32)
	{
		uint32_t lba = chain_lba(first, nofat, off);

		if (!lba)
			return -1;
		if (lba != cur)
		{
			if (rd(lba, 1, b))
				return -1;
			cur = lba;
		}
		if (cb(b + off % SEC, off, arg))
			return 0;
	}
	return 0;
}

/* the root folder's length: its FAT chain, counted */
static uint64_t chain_len(uint32_t first)
{
	uint64_t n = 0;
	uint32_t c = first;

	while (c >= 2 && c < nclus + 2 && n <= nclus)
	{
		n++;
		c = fat_next(c);
	}
	return n << (spc_shift + 9);
}

struct root_scan
{
	int got_bmp, got_up;
	uint32_t up_clus;
	uint64_t up_len;
};

static int root_cb(const uint8_t *e, uint64_t off, void *arg)
{
	struct root_scan *r = (struct root_scan *)arg;

	(void)off;
	if (e[0] == 0)
		return 1;
	if (e[0] == 0x81 && !(e[1] & 1) && !r->got_bmp)
	{
		bmp_clus = le32(e + 20);
		bmp_len = le64(e + 24);
		r->got_bmp = 1;
	}
	if (e[0] == 0x82 && !r->got_up)
	{
		r->up_clus = le32(e + 20);
		r->up_len = le64(e + 24);
		r->got_up = 1;
	}
	return 0;
}

static int load_upcase(uint32_t first, uint64_t len)
{
	uint8_t *raw;
	uint64_t off;
	uint32_t i = 0, k;

	if (!upcase)
		upcase = (uint16_t *)malloc(65536 * sizeof(uint16_t));
	if (!upcase || len > 0x20000 || len < 2)
		return -1;
	raw = (uint8_t *)malloc((size_t)len + SEC);
	if (!raw)
		return -1;
	for (off = 0; off < len; off += SEC)
	{
		uint32_t lba = chain_lba(first, 0, off);

		if (!lba || rd(lba, 1, raw + off))
		{
			free(raw);
			return -1;
		}
	}
	for (k = 0; k < 65536; k++)
		upcase[k] = (uint16_t)k;
	/* compressed form: 0xFFFF n = the next n characters map to themselves */
	for (off = 0; off + 1 < len && i < 65536; off += 2)
	{
		uint32_t w = le16(raw + off);

		if (w == 0xFFFF && off + 3 < len)
		{
			off += 2;
			i += le16(raw + off);
		}
		else
			upcase[i++] = (uint16_t)w;
	}
	free(raw);
	return 0;
}

static uint16_t up(uint16_t c)
{
	return upcase ? upcase[c] : (c >= 'a' && c <= 'z' ? (uint16_t)(c - 32) : c);
}

/* UTF-8 -> UTF-16 (BMP only); -1 = not valid for an exFAT name */
static int to_u16(const char *s, uint16_t *out, int max)
{
	int n = 0;
	const uint8_t *p = (const uint8_t *)s;

	while (*p)
	{
		uint32_t c;

		if (*p < 0x80)
			c = *p++;
		else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
		{
			c = (uint32_t)(*p & 0x1F) << 6 | (p[1] & 0x3F);
			p += 2;
		}
		else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
		{
			c = (uint32_t)(*p & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
			p += 3;
		}
		else
			return -1;
		if (c < 0x20 || strchr("\"*/:<>?\\|", (int)c) || n >= max)
			return -1;
		out[n++] = (uint16_t)c;
	}
	return n;
}

/* the entry set's File + Stream + Name entries in one folder, matched by name (up-cased) */
struct find_name
{
	const uint16_t *w;
	int n;
	int state;		/* 0 looking, 1 in a set */
	int secs_left, dir;
	int got;		/* matched */
	uint32_t clus;
	uint64_t len;
	int nofat;
	int name_len, name_at;
	uint16_t name[256];
	uint64_t set_off, got_off;	/* v0.73: the File entry of the set being read / of the match, bytes into the folder */
	int set_n, got_n;		/* entries in that set, the File entry included */
};

static int name_cb(const uint8_t *e, uint64_t off, void *arg)
{
	struct find_name *f = (struct find_name *)arg;
	int k;

	if (e[0] == 0)
		return 1;
	if (e[0] == 0x85)
	{
		f->state = 1;
		f->secs_left = e[1];
		f->dir = (le16(e + 4) & 0x10) != 0;
		f->name_at = 0;
		f->set_off = off;
		f->set_n = e[1] + 1;
		return 0;
	}
	if (f->state != 1 || !(e[0] & 0x80))
	{
		f->state = 0;
		return 0;
	}
	if (e[0] == 0xC0)
	{
		f->name_len = e[3];
		f->clus = le32(e + 20);
		f->len = le64(e + 24);
		f->nofat = (e[1] & 2) != 0;
	}
	else if (e[0] == 0xC1)
		for (k = 0; k < 15 && f->name_at < f->name_len && f->name_at < 255; k++)
			f->name[f->name_at++] = (uint16_t)le16(e + 2 + 2 * k);
	if (--f->secs_left == 0)
	{
		f->state = 0;
		if (f->name_len == f->n && f->name_at == f->n)
		{
			for (k = 0; k < f->n && up(f->name[k]) == up(f->w[k]); k++)
				;
			if (k == f->n)
			{
				f->got = 1;
				f->got_off = f->set_off;
				f->got_n = f->set_n;
				return 1;
			}
		}
	}
	return 0;
}

/* What every change starts with: the volume, its up-case table, its allocation bitmap (whole, in memory)
 * and the folder (one level under the root). Nothing is written. v0.73: out of exw_begin, for exw_delete. */
static int prep(const exw_dev *dev, const char *dir, char *why, int wn)
{
	struct root_scan rs;
	struct find_name fd;
	uint16_t wdir[256];
	int ndir;
	uint64_t i;

	exw_abort();
	D = *dev;
	fb_lba = 0;
	say(why, wn, "");
	if (find_volume(why, wn))
		return -1;
	memset(&rs, 0, sizeof rs);
	if (walk(root_clus, 0, chain_len(root_clus), root_cb, &rs) || !rs.got_bmp || !rs.got_up)
	{
		say(why, wn, "the exFAT volume's root folder has no allocation bitmap or up-case table");
		return -1;
	}
	if (bmp_len < (nclus + 7) / 8 || bmp_len > 64u << 20 || load_upcase(rs.up_clus, rs.up_len))
	{
		say(why, wn, "could not read the exFAT allocation bitmap or up-case table");
		return -1;
	}
	if (!dir)
	{
		/* v0.73: the root folder itself (exw_space) */
		dir_clus = root_clus;
		dir_len = chain_len(root_clus);
		dir_nofat = 0;
	}
	else
	{
		ndir = to_u16(dir, wdir, 255);
		if (ndir <= 0)
		{
			say(why, wn, "the name cannot be used on exFAT");
			return -1;
		}
		/* the folder */
		memset(&fd, 0, sizeof fd);
		fd.w = wdir;
		fd.n = ndir;
		if (walk(root_clus, 0, chain_len(root_clus), name_cb, &fd) || !fd.got || !fd.dir || fd.clus < 2)
		{
			say(why, wn, "the folder is not on the card's exFAT part");
			return -1;
		}
		dir_clus = fd.clus;
		dir_len = fd.len;
		dir_nofat = fd.nofat;
	}
	/* the bitmap, whole */
	free(bmp);
	bmp = (uint8_t *)malloc((size_t)bmp_len + SEC);
	if (!bmp)
	{
		say(why, wn, "out of memory");
		return -1;
	}
	for (i = 0; i < bmp_len; i += SEC)
	{
		uint32_t lba = chain_lba(bmp_clus, 0, i);

		if (!lba || rd(lba, 1, bmp + i))
		{
			say(why, wn, "could not read the allocation bitmap");
			return -1;
		}
	}
	return 0;
}

int exw_begin(const exw_dev *dev, const char *dir, const char *name, uint64_t size, char *why, int wn)
{
	struct find_name fd;
	uint64_t need, i, run = 0, best = 0;

	if (prep(dev, dir, why, wn))
		return -1;
	ulen = to_u16(name, uname, 255);
	if (ulen <= 0)
	{
		say(why, wn, "the name cannot be used on exFAT");
		return -1;
	}
	/* the name must be new there */
	memset(&fd, 0, sizeof fd);
	fd.w = uname;
	fd.n = ulen;
	if (walk(dir_clus, dir_nofat, dir_len, name_cb, &fd))
	{
		say(why, wn, "could not read the folder");
		return -1;
	}
	if (fd.got)
	{
		say(why, wn, "a file with that name is already there");
		return -1;
	}
	/* one free run for the whole file: the first that fits */
	need = (size + ((uint64_t)SEC << spc_shift) - 1) >> (spc_shift + 9);
	if (!need)
		need = 1;
	f_n = 0;
	for (i = 0; i < nclus; i++)
	{
		if (bmp[i >> 3] & (1u << (i & 7)))
		{
			run = 0;
			continue;
		}
		if (++run > best)
			best = run;
		if (run == need)
		{
			f_clus = (uint32_t)(i + 1 - need) + 2;
			f_n = (uint32_t)need;
			break;
		}
	}
	if (!f_n)
	{
		uint64_t fr = exw_free_bytes();

		if (fr < (need << (spc_shift + 9)))
		{
			char m[120];

			snprintf(m, sizeof m, "the card's exFAT part has %llu MB free; this needs %llu MB",
				(unsigned long long)(fr >> 20), (unsigned long long)((need << (spc_shift + 9)) >> 20));
			say(why, wn, m);
		}
		else
			say(why, wn, "no single free stretch on the card's exFAT part is big enough for it (free space is in pieces)");
		return -1;
	}
	f_size = size;
	f_done = 0;
	active = 1;
	return 0;
}

uint64_t exw_free_bytes(void)
{
	uint64_t i, n = 0;

	if (!bmp)
		return 0;
	for (i = 0; i < nclus; i++)
		if (!(bmp[i >> 3] & (1u << (i & 7))))
			n++;
	return n << (spc_shift + 9);
}

int exw_write(const void *buf, uint32_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	uint32_t whole = len / SEC, lba;
	int r;

	if (!active || f_done % SEC || f_done + len > f_size)
		return -1;
	lba = clus_lba(f_clus) + (uint32_t)(f_done / SEC);
	if (whole)
	{
		r = wr(lba, whole, p);
		if (r)
			return r;
	}
	if (len % SEC)
	{
		uint8_t last[SEC];

		memset(last, 0, sizeof last);
		memcpy(last, p + whole * SEC, len % SEC);
		r = wr(lba + whole, 1, last);
		if (r)
			return r;
	}
	f_done += len;
	return 0;
}

/* free slots in the folder for count entries in a row */
struct slot_find
{
	int need, run;
	uint64_t at;
	int got;
};

static int slot_cb(const uint8_t *e, uint64_t off, void *arg)
{
	struct slot_find *s = (struct slot_find *)arg;

	if (!(e[0] & 0x80))
	{
		if (!s->run)
			s->at = off;
		if (++s->run == s->need)
		{
			s->got = 1;
			return 1;
		}
	}
	else
		s->run = 0;
	return 0;
}

static uint16_t set_sum(const uint8_t *e, int count)
{
	uint16_t sum = 0;
	int i;

	for (i = 0; i < count * 32; i++)
	{
		if (i == 2 || i == 3)
			continue;
		sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + e[i]);
	}
	return sum;
}

static uint16_t name_hash(void)
{
	uint16_t h = 0;
	int i;

	for (i = 0; i < ulen; i++)
	{
		uint16_t c = up(uname[i]);

		h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c & 0xFF));
		h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c >> 8));
	}
	return h;
}

int exw_finish(char *why, int wn)
{
	uint8_t set[32 * 20];
	int names = (ulen + 14) / 15, count = 2 + names, i, k;
	struct slot_find sf;
	uint64_t first_byte, last_byte, b;
	uint32_t stamp = (uint32_t)(2026 - 1980) << 25 | 1u << 21 | 1u << 16;	/* 2026-01-01 00:00 */

	if (!active || f_done != f_size)
	{
		say(why, wn, "the file is not whole");
		return -1;
	}
	/* 1. the clusters are used */
	for (b = 0; b < f_n; b++)
	{
		uint64_t c = (uint64_t)(f_clus - 2) + b;

		bmp[c >> 3] |= (uint8_t)(1u << (c & 7));
	}
	first_byte = ((uint64_t)(f_clus - 2) >> 3) / SEC * SEC;
	last_byte = ((uint64_t)(f_clus - 2 + f_n - 1) >> 3) / SEC * SEC;
	for (b = first_byte; b <= last_byte; b += SEC)
	{
		uint32_t lba = chain_lba(bmp_clus, 0, b);

		if (!lba || wr(lba, 1, bmp + b))
		{
			say(why, wn, "could not write the allocation bitmap");
			active = 0;
			return -2;
		}
	}
	/* 2. the folder entry set */
	memset(set, 0, sizeof set);
	set[0] = 0x85;
	set[1] = (uint8_t)(count - 1);
	put16(set + 4, 0x0020);			/* archive */
	put32(set + 8, stamp);
	put32(set + 12, stamp);
	put32(set + 16, stamp);
	set[32] = 0xC0;
	set[33] = 0x03;				/* allocation possible, no FAT chain */
	set[35] = (uint8_t)ulen;
	put16(set + 36, name_hash());
	put64(set + 40, f_size);		/* valid data length */
	put32(set + 52, f_clus);
	put64(set + 56, f_size);		/* data length */
	for (i = 0; i < names; i++)
	{
		uint8_t *e = set + 64 + 32 * i;

		e[0] = 0xC1;
		for (k = 0; k < 15 && i * 15 + k < ulen; k++)
			put16(e + 2 + 2 * k, uname[i * 15 + k]);
	}
	put16(set + 2, set_sum(set, count));
	memset(&sf, 0, sizeof sf);
	sf.need = count;
	if (walk(dir_clus, dir_nofat, dir_len, slot_cb, &sf) || !sf.got)
	{
		say(why, wn, "the folder has no room for another entry (the data is written but not named)");
		active = 0;
		return -3;
	}
	/* the sectors of the set, last first: the File entry, which makes the set count, goes last */
	for (i = count - 1; i >= 0; )
	{
		uint32_t lba = chain_lba(dir_clus, dir_nofat, sf.at + 32 * (uint64_t)i);
		int j;

		if (!lba || rd(lba, 1, sbuf))
		{
			say(why, wn, "could not read the folder");
			active = 0;
			return -4;
		}
		/* every entry of the set that falls in this sector */
		for (j = i; j >= 0 && chain_lba(dir_clus, dir_nofat, sf.at + 32 * (uint64_t)j) == lba; j--)
			memcpy(sbuf + (sf.at + 32 * (uint64_t)j) % SEC, set + 32 * j, 32);
		if (wr(lba, 1, sbuf))
		{
			say(why, wn, "could not write the folder entry");
			active = 0;
			return -5;
		}
		i = j;
	}
	active = 0;
	return 0;
}

void exw_abort(void)
{
	active = 0;
}

/* v0.73: take one file out of a folder (Bananafuzzbuddy, 11-25: a game on a card's exFAT part could be seen and
 * copied but not deleted). Per the exFAT specification a file is gone when its entry set is no longer "in
 * use" (the top bit of each entry's type cleared) and its clusters are free in the allocation bitmap; FAT
 * entries of free clusters mean nothing and are left as they are.
 *
 * Everything is checked before anything is written: the name is there and is a file, and every cluster of
 * the file is inside the volume and marked used (for a FAT chain: as many clusters as its size says, then
 * the end mark). A file that fails this is not touched: the volume disagrees with itself, and a PC's disk
 * check should look at it first.
 * Order: the entry set (File entry first: a set without it is skipped by every reader), then the bitmap.
 * Stopped between the two, the file is gone and its clusters are still marked used: space a disk check
 * frees, never space in use that is marked free. */
int exw_delete(const exw_dev *dev, const char *dir, const char *name, char *why, int wn)
{
	struct find_name fd;
	uint64_t csz, need, k, b;
	uint32_t c, lba;
	uint8_t *dirty;
	int i, j;

	if (prep(dev, dir, why, wn))
		return -1;
	csz = (uint64_t)SEC << spc_shift;
	ulen = to_u16(name, uname, 255);
	if (ulen <= 0)
	{
		say(why, wn, "the name cannot be used on exFAT");
		return -1;
	}
	memset(&fd, 0, sizeof fd);
	fd.w = uname;
	fd.n = ulen;
	if (walk(dir_clus, dir_nofat, dir_len, name_cb, &fd))
	{
		say(why, wn, "could not read the folder");
		return -1;
	}
	if (!fd.got)
	{
		say(why, wn, "no file with that name is there");
		return -1;
	}
	if (fd.dir)
	{
		say(why, wn, "that is a folder, not a file");
		return -1;
	}
	if (fd.got_n < 3 || fd.got_n > 19)
	{
		say(why, wn, "the file's folder entry is not one this writer handles");
		return -1;
	}
	/* 1. its clusters, all looked at first */
	need = (fd.len + csz - 1) / csz;
	c = fd.clus;
	for (k = 0; k < need; k++)
	{
		if (c < 2 || c >= nclus + 2 || !(bmp[(c - 2) >> 3] & (1u << ((c - 2) & 7))))
		{
			say(why, wn, "the file's clusters do not agree with the volume's allocation bitmap: check the card on a PC first");
			return -1;
		}
		if (k + 1 < need)
			c = fd.nofat ? c + 1 : fat_next(c);
	}
	if (need && !fd.nofat && fat_next(c) != 0xFFFFFFFF)
	{
		say(why, wn, "the file's cluster chain does not end where its size says: check the card on a PC first");
		return -1;
	}
	dirty = (uint8_t *)calloc(1, (size_t)(bmp_len / SEC) + 2);
	if (!dirty)
	{
		say(why, wn, "out of memory");
		return -1;
	}
	/* the set as it is on the card now must be the set that was matched */
	for (i = 0; i < fd.got_n; i++)
	{
		uint8_t want = i == 0 ? 0x85 : i == 1 ? 0xC0 : 0xC1;

		lba = chain_lba(dir_clus, dir_nofat, fd.got_off + 32 * (uint64_t)i);
		if (!lba || rd(lba, 1, sbuf) || sbuf[(fd.got_off + 32 * (uint64_t)i) % SEC] != want)
		{
			free(dirty);
			say(why, wn, "the file's folder entry is not one this writer handles");
			return -1;
		}
	}
	/* 2. the entry set is no longer in use */
	for (i = 0; i < fd.got_n; i = j)
	{
		lba = chain_lba(dir_clus, dir_nofat, fd.got_off + 32 * (uint64_t)i);
		if (!lba || rd(lba, 1, sbuf))
		{
			free(dirty);
			say(why, wn, i ? "could not read the folder (the file is gone; part of its entry is left for a disk check)" : "could not read the folder");
			return i ? -2 : -1;
		}
		/* every entry of the set that falls in this sector */
		for (j = i; j < fd.got_n && chain_lba(dir_clus, dir_nofat, fd.got_off + 32 * (uint64_t)j) == lba; j++)
			sbuf[(fd.got_off + 32 * (uint64_t)j) % SEC] &= 0x7F;
		if (wr(lba, 1, sbuf))
		{
			free(dirty);
			say(why, wn, i ? "could not write the folder (the file is gone; part of its entry is left for a disk check)" : "could not write the folder: nothing was deleted");
			return i ? -2 : -1;
		}
	}
	/* 3. its clusters are free */
	c = fd.clus;
	for (k = 0; k < need; k++)
	{
		uint64_t bit = (uint64_t)c - 2;

		bmp[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
		dirty[(bit >> 3) / SEC] = 1;
		if (k + 1 < need)
			c = fd.nofat ? c + 1 : fat_next(c);
	}
	for (b = 0; b < bmp_len; b += SEC)
	{
		if (!dirty[b / SEC])
			continue;
		lba = chain_lba(bmp_clus, 0, b);
		if (!lba || wr(lba, 1, bmp + b))
		{
			free(dirty);
			say(why, wn, "the file is deleted, but its space could not be marked free (a disk check on a PC frees it)");
			return -2;
		}
	}
	free(dirty);
	return 0;
}

/* v0.73: how big the exFAT volume is and how much of it is free (SETTINGS' storage list: a card that is
 * exFAT only is not mounted by the PS3, so the system cannot say). Nothing is written. */
int exw_space(const exw_dev *dev, uint64_t *free_b, uint64_t *total_b, char *why, int wn)
{
	if (prep(dev, NULL, why, wn))
		return -1;
	*free_b = exw_free_bytes();
	*total_b = (uint64_t)nclus << (spc_shift + 9);
	return 0;
}
