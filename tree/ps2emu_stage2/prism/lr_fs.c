/* LR-G5: read-only FAT32 and exFAT, just enough to open one file by path and read it.
 *
 * The file's cluster chain is walked ONCE, at open, into a list of sector runs. After that a
 * read is arithmetic plus one READ(10); no FAT access happens while the game runs.
 *
 * All I/O goes through two windows in the DMA block: LR_OFF_META (directory and FAT sectors)
 * and LR_OFF_DATA (file data, doubling as read-ahead for the sequential reads games make).
 */
#include <ps2emu/ps2emu.h>
#include <ps2emu/libc.h>
#include "lr.h"

#define META_SECS	(LR_META_SZ / 512)
#define DATA_SECS	(LR_DATA_SZ / 512)
#define MAX_EXT		2048
#define FS_FAT32	1
#define FS_EXFAT	2
#define FS_FAT16	3

typedef struct
{
	u64 size;	/* bytes; 0 = unknown (FAT32 directory) */
	u32 clus;
	u8 nofat;	/* exFAT: contiguous, the FAT holds no chain for it */
	u8 dir;
	u8 vol;		/* index into vols[] */
} node;

typedef struct
{
	u32 lba;
	u32 n;		/* sectors */
} ext;

typedef struct
{
	const node *nd;
	u32 clus, cidx, sec, ent;
} diter;

typedef struct
{
	u8 type, shift;		/* shift = log2(sectors per cluster) */
	u32 part, fat_lba, data_lba, nclus, root;
} vol_t;

/* every volume on the card; fs is a copy of the one being worked on */
static vol_t vols[LR_FS_VOLS], fs;
static u32 n_vol;

static u32 meta_lba, meta_n;
static ext *exts;
static u32 n_ext, cur_ext;
static u64 ext_secs;	/* sectors in exts[] */
static u64 cur_base, file_size, win_off;
static u32 win_len;
static u16 lfn[256];

/* LR-MENU: the games on the card, kept so that an entry can be opened by its number with no
 * second walk of the directory. */
#define CAT_NAME	104
typedef struct
{
	node nd;		/* the file; a split game: its part 0, with the size of all parts */
	node dirn;		/* the directory it is in */
	char name[CAT_NAME];	/* UTF-8, NUL-terminated; a split game's name without ".0" */
	char id[12];		/* title ID from the image's SYSTEM.CNF, or empty */
	u8 kind;		/* 0 DVD, 1 CD (2048-byte sectors), 2 CD (raw 2352-byte image) */
	u8 split;		/* the parts are name.0, name.1, ... in dirn */
} cat_ent;

static cat_ent *cat;
static u32 n_cat;
static node cat_dir;	/* the directory being listed */
u8 lr_cat_more;		/* the directory holds more games than LR_CAT_MAX */

static u32 le16(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8);
}

/* noinline: left to itself gcc 13 fuses two of these into ldbrx, a doubleword byte-reverse
 * load that is not part of the base instruction set. emu/harness.py's audit refuses it. */
static u32 __attribute__((noinline)) le32(const u8 *p)
{
	return le16(p) | (le16(p + 2) << 16);
}

static u64 le64(const u8 *p)
{
	return (u64)le32(p) | ((u64)le32(p + 4) << 32);
}

static u8 *meta(u32 lba)
{
	if (!meta_n || lba < meta_lba || lba >= meta_lba + meta_n)
	{
		u32 n = META_SECS;

		meta_n = 0;
		if (lba >= lr_blk_count)
			return NULL;
		if (n > lr_blk_count - lba)
			n = lr_blk_count - lba;
		if (lr_blk_read(lba, n, LR_OFF_META))
			return NULL;
		meta_lba = lba;
		meta_n = n;
	}

	return lr_dma + LR_OFF_META + (lba - meta_lba) * 512;
}

void lr_fs_forget(void)
{
	meta_n = 0;
	win_len = 0;
}

static int vbr_type(const u8 *s)
{
	if (memcmp(s + 3, "EXFAT   ", 8) == 0)
		return FS_EXFAT;
	if ((s[0] != 0xEB && s[0] != 0xE9) || le16(s + 11) != 512 || !s[13] || (s[13] & (s[13] - 1)) || !s[16])
		return 0;
	if (le16(s + 17) == 0 && le16(s + 22) == 0 && le32(s + 36))
		return FS_FAT32;

	return FS_FAT16;
}

/* The volume whose boot sector is at part. 0 = *v filled in. */
static int vol_at(u32 part, vol_t *v)
{
	u8 *s = meta(part);
	int t = s ? vbr_type(s) : 0;

	if (t != FS_FAT32 && t != FS_EXFAT)
	{
		lr_log("fs: partition at %x is %s, not read\n", part, t == FS_FAT16 ? "FAT12/16" : "not FAT");
		return LR_E_STATE;
	}

	v->part = part;
	if (t == FS_FAT32)
	{
		u32 spc = s[13], tot = le32(s + 32);

		for (v->shift = 0; (1u << v->shift) < spc; v->shift++)
			;
		v->fat_lba = part + le16(s + 14);
		v->data_lba = v->fat_lba + s[16] * le32(s + 36);
		v->root = le32(s + 44);
		if (!tot)
			tot = le16(s + 19);
		if (tot <= v->data_lba - part)
			return LR_E_STATE;
		v->nclus = (tot - (v->data_lba - part)) >> v->shift;
	}
	else
	{
		if (s[108] != 9 || s[109] > 16)
		{
			lr_log("fs: exFAT with %d-byte sectors is not supported\n", 1 << s[108]);
			return LR_E_STATE;
		}
		v->shift = s[109];
		v->fat_lba = part + le32(s + 80);
		v->data_lba = part + le32(s + 88);
		v->nclus = le32(s + 92);
		v->root = le32(s + 96);
	}

	if (v->root < 2 || v->root >= v->nclus + 2)
	{
		lr_log("fs: root cluster %x out of range\n", v->root);
		return LR_E_STATE;
	}

	v->type = (u8)t;
	lr_log("fs: %s part %x fat %x data %x clusters %x of %d sectors root %x\n", t == FS_FAT32 ? "FAT32" : "exFAT", v->part,
		v->fat_lba, v->data_lba, v->nclus, 1 << v->shift, v->root);

	return 0;
}

static void use_vol(u32 v)
{
	memcpy(&fs, &vols[v], sizeof(fs));
}

/* Every FAT32 / exFAT volume on the card: sector 0 itself, or the four primary MBR entries, or
 * the first four GPT entries. A partition of another kind is said in the log and passed over. */
int lr_fs_mount(void)
{
	u32 start[LR_FS_VOLS], n = 0, i;
	u8 *s;

	fs.type = 0;
	n_vol = 0;
	lr_fs_forget();

	s = meta(0);
	if (!s)
		return LR_E_STATE;
	if (s[510] != 0x55 || s[511] != 0xAA)
	{
		lr_log("fs: sector 0 has no 55AA signature (%02x%02x)\n", s[510], s[511]);
		return LR_E_STATE;
	}

	if (vbr_type(s))
		start[n++] = 0;
	else
	{
		for (i = 0; i < 4; i++)
		{
			const u8 *e = s + 446 + 16 * i;

			if (e[4] == 0xEE && !n)
			{
				/* GPT: the entries of its first four that have a start LBA */
				u8 *g = meta(1);
				u64 el;
				int k;

				if (!g || memcmp(g, "EFI PART", 8) != 0)
					break;
				el = le64(g + 72);
				if (el >> 32)
					break;
				g = meta((u32)el);
				for (k = 0; g && k < 4; k++)
				{
					u64 first = le64(g + 128 * k + 32);

					if (first && !(first >> 32))
						start[n++] = (u32)first;
				}
				break;
			}
			if (e[4] && e[4] != 0xEE && le32(e + 8))
				start[n++] = le32(e + 8);
		}
	}

	if (!n)
	{
		lr_log("fs: no partition found\n");
		return LR_E_STATE;
	}

	for (i = 0; i < n; i++)
	{
		if (vol_at(start[i], &vols[n_vol]) == 0)
			n_vol++;
	}

	if (!n_vol)
	{
		lr_log("fs: no FAT32 or exFAT volume; format the card FAT32 or exFAT\n");
		return LR_E_STATE;
	}

	use_vol(0);
	return 0;
}

static u32 clus_lba(u32 c)
{
	return fs.data_lba + ((c - 2) << fs.shift);
}

/* next cluster in the FAT. 0 = end of chain, 1 = error. */
static u32 fat_next(u32 c)
{
	u8 *s;
	u32 v;

	if (c < 2 || c >= fs.nclus + 2)
		return 1;

	s = meta(fs.fat_lba + (c >> 7));
	if (!s)
		return 1;

	v = le32(s + (c & 127) * 4);
	if (fs.type == FS_FAT32)
	{
		v &= 0x0fffffff;
		if (v >= 0x0ffffff8)
			return 0;
	}
	else if (v == 0xffffffff)
		return 0;

	if (v < 2 || v >= fs.nclus + 2)
		return 1;

	return v;
}

/* idx = position of c in its chain (0 for the first cluster) */
static u32 chain_next(const node *nd, u32 c, u32 idx)
{
	if (nd->nofat)
		return ((u64)(idx + 1) << (fs.shift + 9)) < nd->size ? c + 1 : 0;

	return fat_next(c);
}

/* ------------------------------------------------------------- directories */

static void dir_begin(diter *it, const node *nd)
{
	it->nd = nd;
	it->clus = nd->clus;
	it->cidx = 0;
	it->sec = 0;
	it->ent = 0;
}

/* next 32-byte entry, or NULL at the end of the chain / on a read error. The pointer is
 * valid only until the next call. */
static u8 *dir_next(diter *it)
{
	u8 *s;

	if (it->ent == 16)
	{
		it->ent = 0;
		if (++it->sec == (1u << fs.shift))
		{
			u32 n = chain_next(it->nd, it->clus, it->cidx);

			it->sec = 0;
			it->clus = n;
			if (++it->cidx > 0x4000)	/* a looping chain must not hang the console */
				it->clus = 0;
		}
	}

	if (it->clus < 2 || it->clus >= fs.nclus + 2)
		return NULL;

	s = meta(clus_lba(it->clus) + it->sec);
	if (!s)
		return NULL;

	return s + 32 * it->ent++;
}

static u32 up(u32 c)
{
	return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

/* UTF-16 name of n units against a UTF-8 path component; ASCII letters compare caseless. */
static int name_eq(const u16 *w, int n, const u8 *t, int tlen)
{
	int i = 0, j = 0;

	while (j < tlen)
	{
		u32 c = t[j++];

		if (c >= 0xf0)
			return 0;
		if (c >= 0xe0)
		{
			if (j + 2 > tlen)
				return 0;
			c = ((c & 15) << 12) | ((t[j] & 63u) << 6) | (t[j + 1] & 63u);
			j += 2;
		}
		else if (c >= 0xc0)
		{
			if (j + 1 > tlen)
				return 0;
			c = ((c & 31) << 6) | (t[j] & 63u);
			j++;
		}

		if (i >= n || up(w[i]) != up(c))
			return 0;
		i++;
	}

	return i == n;
}

/* Listing: take a file whose name ends .iso or .bin, or .iso.0 / .bin.0 (part 0 of a split
 * game, listed without the ".0"). Names name_eq() could not match again (outside the 16-bit
 * plane) or that do not fit a record are left out. */
static void cat_add(const u16 *w, int n, const node *nd)
{
	char *o;
	int i, j = 0, split = 0;
	u32 a, b, c;

	if (nd->dir || !nd->size || n < 5 || w[0] == '.')
		return;
	if (n >= 7 && w[n - 2] == '.' && w[n - 1] == '0')
	{
		split = 1;
		n -= 2;
	}
	if (w[n - 4] != '.')
		return;
	a = up(w[n - 3]);
	b = up(w[n - 2]);
	c = up(w[n - 1]);
	if (!(a == 'I' && b == 'S' && c == 'O') && !(a == 'B' && b == 'I' && c == 'N'))
		return;
	if (n_cat == LR_CAT_MAX)
	{
		lr_cat_more = 1;
		return;
	}

	o = cat[n_cat].name;
	for (i = 0; i < n; i++)
	{
		c = w[i];
		if (j > CAT_NAME - 4 || c < 0x20 || (c >= 0xD800 && c < 0xE000))
			return;
		if (c < 0x80)
			o[j++] = (char)c;
		else
		{
			if (c < 0x800)
				o[j++] = (char)(0xC0 | (c >> 6));
			else
			{
				o[j++] = (char)(0xE0 | (c >> 12));
				o[j++] = (char)(0x80 | ((c >> 6) & 63));
			}
			o[j++] = (char)(0x80 | (c & 63));
		}
	}
	o[j] = 0;
	memset(cat[n_cat].id, 0, 12);
	cat[n_cat].kind = 0;
	cat[n_cat].split = (u8)split;
	cat[n_cat].dirn = cat_dir;
	cat[n_cat++].nd = *nd;
}

/* t == NULL lists the directory into the catalogue instead of looking one name up. */
static int fat_lookup(const node *dir, const u8 *t, int tlen, node *out)
{
	static const u8 offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
	diter it;
	u8 *e;
	int lfn_n = 0, lfn_seq = 0, k;
	u8 lfn_sum = 0;
	u16 s83[13];

	dir_begin(&it, dir);
	while ((e = dir_next(&it)) != NULL)
	{
		const u16 *nm = NULL;
		int match = 0, nn = 0;
		node nd;

		if (e[0] == 0)
			break;
		if (e[0] == 0xE5)
		{
			lfn_n = 0;
			continue;
		}

		if ((e[11] & 0x3f) == 0x0f)
		{
			int seq = e[0] & 0x3f;

			if (seq < 1 || seq > 19)
			{
				lfn_n = 0;
				continue;
			}
			if (e[0] & 0x40)
			{
				lfn_n = seq * 13;
				lfn_sum = e[13];
			}
			else if (!lfn_n || seq != lfn_seq - 1 || e[13] != lfn_sum)
			{
				lfn_n = 0;
				continue;
			}
			lfn_seq = seq;
			for (k = 0; k < 13; k++)
				lfn[(seq - 1) * 13 + k] = (u16)le16(e + offs[k]);
			continue;
		}

		if (e[11] & 0x08)
		{
			lfn_n = 0;
			continue;
		}

		if (lfn_n && lfn_seq == 1)
		{
			u8 sum = 0;

			for (k = 0; k < 11; k++)
				sum = (u8)(((sum & 1) << 7) + (sum >> 1) + e[k]);
			if (sum == lfn_sum)
			{
				while (nn < lfn_n && lfn[nn])
					nn++;
				nm = lfn;
				match = t && name_eq(lfn, nn, t, tlen);
			}
		}
		lfn_n = 0;

		if (!match)
		{
			int n = 0, last;

			for (last = 7; last >= 0 && e[last] == ' '; last--)
				;
			for (k = 0; k <= last; k++)
				s83[n++] = (k == 0 && e[0] == 0x05) ? 0xE5 : e[k];
			for (last = 10; last >= 8 && e[last] == ' '; last--)
				;
			if (last >= 8)
			{
				s83[n++] = '.';
				for (k = 8; k <= last; k++)
					s83[n++] = e[k];
			}
			if (!nm)
			{
				nm = s83;
				nn = n;
			}
			match = t && name_eq(s83, n, t, tlen);
		}

		nd.clus = (le16(e + 20) << 16) | le16(e + 26);
		nd.size = le32(e + 28);
		nd.dir = (e[11] & 0x10) != 0;
		nd.nofat = 0;
		nd.vol = dir->vol;
		if (!t)
			cat_add(nm, nn, &nd);
		else if (match)
		{
			*out = nd;
			return 0;
		}
	}

	return LR_E_NOFILE;
}

static int exfat_lookup(const node *dir, const u8 *t, int tlen, node *out)
{
	diter it;
	u8 *e;
	node cand;
	int st = 0, isdir = 0, nlen = 0, got = 0, k;

	cand.size = 0;
	cand.clus = 0;
	cand.nofat = 0;
	cand.dir = 0;
	cand.vol = dir->vol;

	dir_begin(&it, dir);
	while ((e = dir_next(&it)) != NULL)
	{
		u8 ty = e[0];

		if (ty == 0)
			break;

		if (ty == 0x85)
		{
			st = 1;
			isdir = (le16(e + 4) & 0x10) != 0;
		}
		else if (ty == 0xC0 && st == 1)
		{
			cand.nofat = (e[1] & 2) != 0;
			cand.clus = le32(e + 20);
			cand.size = le64(e + 24);
			cand.dir = (u8)isdir;
			nlen = e[3];
			got = 0;
			st = nlen ? 2 : 0;
		}
		else if (ty == 0xC1 && st == 2)
		{
			for (k = 0; k < 15 && got < nlen; k++)
				lfn[got++] = (u16)le16(e + 2 + 2 * k);
			if (got == nlen)
			{
				st = 0;
				if (!t)
					cat_add(lfn, nlen, &cand);
				else if (name_eq(lfn, nlen, t, tlen))
				{
					out->size = cand.size;
					out->clus = cand.clus;
					out->nofat = cand.nofat;
					out->dir = cand.dir;
					out->vol = cand.vol;
					return 0;
				}
			}
		}
		else
			st = 0;
	}

	return LR_E_NOFILE;
}

/* ------------------------------------------------------------------- files */

/* in the volume fs */
static int lookup(const node *dir, const u8 *t, int tlen, node *out)
{
	return (fs.type == FS_FAT32) ? fat_lookup(dir, t, tlen, out) : exfat_lookup(dir, t, tlen, out);
}

/* Append the sector runs of file f (on the volume fs) to exts[]. The last run is cut to the
 * file's own length, so the next file of a split game follows on at the right byte. */
static int build_extents(const node *f)
{
	u32 spc = 1u << fs.shift;
	u64 cb = (u64)spc << 9;
	u64 need64 = (f->size + cb - 1) >> (fs.shift + 9);
	u64 before = ext_secs, want = (f->size + 511) >> 9;
	u32 need, c = f->clus, idx;

	if (!f->size || need64 > fs.nclus)
	{
		lr_log("fs: file size %lx does not fit the volume\n", (unsigned long)f->size);
		return LR_E_STATE;
	}
	need = (u32)need64;

	if (!exts)
		exts = malloc(MAX_EXT * sizeof(ext));
	if (!exts)
	{
		lr_log("fs: no memory for the extent list\n");
		return LR_E_STATE;
	}

	if (c < 2 || c >= fs.nclus + 2)
		return LR_E_STATE;

	if (f->nofat)
	{
		if (need > fs.nclus + 2 - c)
			return LR_E_STATE;
		if (n_ext == MAX_EXT)
			goto frag;
		exts[n_ext].lba = clus_lba(c);
		exts[n_ext].n = need << fs.shift;
		ext_secs += (u64)need << fs.shift;
		n_ext++;
	}
	else
	{
		u32 first = n_ext;

		for (idx = 0; idx < need; idx++)
		{
			u32 lba = clus_lba(c);

			if (n_ext > first && exts[n_ext - 1].lba + exts[n_ext - 1].n == lba)
				exts[n_ext - 1].n += spc;
			else
			{
				if (n_ext == MAX_EXT)
					goto frag;
				exts[n_ext].lba = lba;
				exts[n_ext].n = spc;
				n_ext++;
			}
			ext_secs += spc;

			if (idx + 1 < need)
			{
				c = fat_next(c);
				if (c < 2)
				{
					lr_log("fs: cluster chain ends at %d of %d\n", (int)idx + 1, (int)need);
					return LR_E_STATE;
				}
			}
		}
	}

	/* the last cluster is only partly the file's: less than one cluster, inside the last run */
	if (ext_secs - before > want)
	{
		u32 cut = (u32)(ext_secs - before - want);

		exts[n_ext - 1].n -= cut;
		ext_secs -= cut;
	}

	for (idx = 0; idx < n_ext; idx++)
	{
		if (exts[idx].lba >= lr_blk_count || exts[idx].n > lr_blk_count - exts[idx].lba)
		{
			lr_log("fs: extent %d runs past the end of the card\n", (int)idx);
			return LR_E_STATE;
		}
	}

	return 0;

frag:
	lr_log("fs: file is in more than %d fragments; copy it to a freshly formatted card\n", MAX_EXT);
	return LR_E_STATE;
}

/* Follow a path from the root of volume v. Says nothing: the caller knows what a miss means. */
static int walk_on(u32 v, const char *path, node *cur)
{
	const u8 *p = (const u8 *)path;
	node nx;
	int r;

	if (v >= n_vol)
		return LR_E_STATE;
	use_vol(v);

	cur->clus = fs.root;
	cur->size = 0;
	cur->nofat = 0;
	cur->dir = 1;
	cur->vol = (u8)v;
	nx = *cur;

	for (;;)
	{
		int n = 0;

		while (*p == '/' || *p == '\\')
			p++;
		if (!*p)
			break;
		while (p[n] && p[n] != '/' && p[n] != '\\')
			n++;

		if (!cur->dir)
			return LR_E_NOFILE;

		r = lookup(cur, p, n, &nx);
		if (r)
			return r;

		*cur = nx;
		p += n;
	}

	return 0;
}

static void ext_reset(void)
{
	file_size = 0;
	n_ext = 0;
	ext_secs = 0;
	cur_ext = 0;
	cur_base = 0;
	win_len = 0;
}

/* exts[] is complete: make it the open file. name NULL = say nothing in the log. */
static void opened(const char *name, u64 total, u32 parts, u64 *size)
{
	file_size = total;
	cur_ext = 0;
	cur_base = 0;
	win_len = 0;
	*size = total;
	if (name && parts)
		lr_log("fs: %s size %lx in %d part(s), %d extent(s), first lba %x\n", name, (unsigned long)total, (int)parts, (int)n_ext,
			exts[0].lba);
	else if (name)
		lr_log("fs: %s size %lx in %d extent(s), first lba %x\n", name, (unsigned long)total, (int)n_ext, exts[0].lba);
}

static int open_node(const node *f, const char *name, u64 *size)
{
	int r;

	ext_reset();
	if (f->dir)
		return LR_E_NOFILE;

	use_vol(f->vol);
	r = build_extents(f);
	if (r)
	{
		n_ext = 0;
		return r;
	}

	opened(name, f->size, 0, size);
	return 0;
}

/* A split game: leaf.0, leaf.1, ... in dir, read end to end. build = 0 only adds up the size.
 * LR_E_NOFILE: there is no leaf.0. LR_E_STATE: there is, but the set cannot be read as one
 * disc (the log says why). */
static int split_open(const node *dir, const char *leaf, const char *name, u64 *size, int build)
{
	char nm[CAT_NAME + 8];
	node part;
	u64 total = 0, last = 0;
	u32 k;
	int n;

	if (build)
		ext_reset();
	use_vol(dir->vol);

	for (k = 0; ; k++)
	{
		n = lr_fmt(nm, sizeof(nm), "%s.%u", leaf, k);
		if (lookup(dir, (const u8 *)nm, n, &part))
			break;
		if (k == LR_SPLIT_MAX)
		{
			lr_log("fs: %s has more than %d parts\n", leaf, LR_SPLIT_MAX);
			goto bad;
		}
		if (part.dir || !part.size)
		{
			lr_log("fs: %s is %s\n", nm, part.dir ? "a folder" : "empty");
			goto bad;
		}
		if (last & 2047)
		{
			lr_log("fs: %s.%u is %lx bytes, not whole 2048-byte sectors; split a game at a multiple of 2048\n", leaf, k - 1,
				(unsigned long)last);
			goto bad;
		}
		if (build && build_extents(&part))
			goto bad;
		total += part.size;
		last = part.size;
	}

	if (!k)
		return LR_E_NOFILE;

	/* a set that stops early because a part in the middle is gone is not the whole disc */
	n = lr_fmt(nm, sizeof(nm), "%s.%u", leaf, k + 1);
	if (lookup(dir, (const u8 *)nm, n, &part) == 0)
	{
		lr_log("fs: %s.%u is missing\n", leaf, k);
		goto bad;
	}

	if (build)
		opened(name, total, k, size);
	else
		*size = total;
	return 0;

bad:
	if (build)
		ext_reset();
	return LR_E_STATE;
}

/* path on volume v, whole or split. LR_E_NOFILE = neither is there. */
static int open_on(u32 v, const char *path, u64 *size)
{
	char parent[208];
	const char *leaf = path, *p;
	node d, f;
	int n;

	for (p = path; *p; p++)
	{
		if (*p == '/' || *p == '\\')
			leaf = p + 1;
	}
	n = (int)(leaf - path);
	if (!*leaf || n >= (int)sizeof(parent))
		return LR_E_NOFILE;
	memcpy(parent, path, n);
	parent[n] = 0;

	if (walk_on(v, parent, &d) || !d.dir)
		return LR_E_NOFILE;
	if (lookup(&d, (const u8 *)leaf, (int)strlen(leaf), &f) == 0)
		return open_node(&f, path, size);

	return split_open(&d, leaf, path, size, 1);
}

/* path on every volume in turn, whole or split; a path in LR_GAME_DIR also in LR_GAME_DIR2. */
int lr_fs_open(const char *path, u64 *size)
{
	char alt[208];
	u32 v, pass;
	int r;

	ext_reset();
	alt[0] = 0;
	if (memcmp(path, LR_GAME_DIR "/", sizeof(LR_GAME_DIR)) == 0)
		lr_fmt(alt, sizeof(alt), LR_GAME_DIR2 "%s", path + sizeof(LR_GAME_DIR) - 1);

	for (pass = 0; pass < 2; pass++)
	{
		const char *p = pass ? alt : path;

		for (v = 0; *p && v < n_vol; v++)
		{
			r = open_on(v, p, size);
			if (r != LR_E_NOFILE)
				return r;
		}
	}

	lr_log("fs: not found: %s\n", path);
	return LR_E_NOFILE;
}

/* LR-MENU: list the games in LR_GAME_DIR and LR_GAME_DIR2 of every volume. Returns how many,
 * or an error when no volume has either folder. */
int lr_fs_list(void)
{
	/* no table of pointers: data words are not relocated in this image (harness reloc_audit) */
	u32 v, d, i, k;
	int found = 0;

	n_cat = 0;
	lr_cat_more = 0;
	if (!cat)
		cat = malloc(LR_CAT_MAX * sizeof(cat_ent));
	if (!cat)
		return LR_E_STATE;

	for (v = 0; v < n_vol; v++)
	{
		for (d = 0; d < 2; d++)
		{
			node dn;
			u32 first = n_cat;

			if (walk_on(v, d ? LR_GAME_DIR2 : LR_GAME_DIR, &dn) || !dn.dir)
				continue;
			found = 1;
			cat_dir = dn;
			lookup(&dn, NULL, 0, &dn);

			/* a split game's size is all of its parts; a set that cannot be read is left out */
			for (i = k = first; i < n_cat; i++)
			{
				if (cat[i].split)
				{
					u64 size;

					if (split_open(&cat[i].dirn, cat[i].name, NULL, &size, 0))
						continue;
					cat[i].nd.size = size;
				}
				if (k != i)
					memcpy(&cat[k], &cat[i], sizeof(cat_ent));
				k++;
			}
			n_cat = k;
		}
	}

	if (!found)
	{
		lr_log("fs: no " LR_GAME_DIR " or " LR_GAME_DIR2 " folder on the card\n");
		return LR_E_NOFILE;
	}

	return (int)n_cat;
}

const char *lr_fs_cat_name(u32 i)
{
	return i < n_cat ? cat[i].name : NULL;
}

/* One 128-byte record: [0] 1 = a game, [4..7] size in 2048-byte sectors (little-endian, the
 * reader is a PS2 program), [2] disc kind (0 DVD, 1 CD, 2 raw CD image), [8..111] name,
 * [112..123] title ID. */
void lr_fs_cat_rec(u32 i, u8 *out)
{
	u32 secs;

	memset(out, 0, 128);
	if (i >= n_cat)
		return;

	secs = (u32)((cat[i].nd.size + 2047) >> 11);
	out[0] = 1;
	out[2] = cat[i].kind;
	out[4] = (u8)secs;
	out[5] = (u8)(secs >> 8);
	out[6] = (u8)(secs >> 16);
	out[7] = (u8)(secs >> 24);
	memcpy(out + 8, cat[i].name, CAT_NAME);
	memcpy(out + 112, cat[i].id, 12);
}

void lr_fs_cat_id(u32 i, const char *id, int kind)
{
	if (i < n_cat)
	{
		memcpy(cat[i].id, id, 12);
		cat[i].kind = (u8)kind;
	}
}

u32 lr_le32(const u8 *p)
{
	return le32(p);
}

int lr_fs_cat_open(u32 i, u64 *size, int quiet)
{
	if (i >= n_cat)
		return LR_E_NOFILE;
	if (cat[i].split)
		return split_open(&cat[i].dirn, cat[i].name, quiet ? NULL : cat[i].name, size, 1);

	return open_node(&cat[i].nd, quiet ? NULL : cat[i].name, size);
}

#ifdef LR_APP
/* the Prism app only (v0.60, src/pspsd.c: a PSP image on the card served to webMAN by its sectors):
 * run i of the open file, in the card's own 512-byte sectors. The number of runs, or -1 past the last.
 * Not compiled into any stage2: LR_APP is defined by app/build_app.sh alone. */
int lr_fs_run(u32 i, u32 *lba, u32 *n)
{
	if (i >= n_ext)
		return -1;
	*lba = exts[i].lba;
	*n = exts[i].n;
	return (int)n_ext;
}
#endif

int lr_file_read(u64 off, u8 *dst, u32 len)
{
	while (len)
	{
		u64 sec;
		u32 n, cnt, rel;
		int r;

		if (off >= file_size)
		{
			memset(dst, 0, len);
			return 0;
		}

		if (win_len && off >= win_off && off < win_off + win_len)
		{
			n = (u32)(win_off + win_len - off);
			if (n > len)
				n = len;
			memcpy(dst, lr_dma + LR_OFF_DATA + (u32)(off - win_off), n);
			dst += n;
			off += n;
			len -= n;
			continue;
		}

		sec = off >> 9;
		if (sec < cur_base)
		{
			cur_ext = 0;
			cur_base = 0;
		}
		while (cur_ext < n_ext && sec >= cur_base + exts[cur_ext].n)
		{
			cur_base += exts[cur_ext].n;
			cur_ext++;
		}
		if (cur_ext >= n_ext)
		{
			cur_ext = 0;
			cur_base = 0;
			return LR_E_STATE;
		}

		rel = (u32)(sec - cur_base);
		cnt = exts[cur_ext].n - rel;
		if (cnt > DATA_SECS)
			cnt = DATA_SECS;

		win_len = 0;
		r = lr_blk_read(exts[cur_ext].lba + rel, cnt, LR_OFF_DATA);
		if (r)
			return r;

		win_off = sec << 9;
		win_len = cnt << 9;
	}

	return 0;
}
