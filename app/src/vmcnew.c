/* v0.79 (Bananafuzzbuddy, 2026-11-25: "allow the creation of new cards, and the renaming of cards").
 *
 * A NEW PS2 CARD is built to be what the PS3 itself makes. One of the owner's cards
 * (_NEW_0600324400893446.VM2, as backed up on 2026-11-22) was an empty card straight from the XMB's
 * Memory Card Utility; host/vm2_blank_facts.py measured it and host/vmcnew_test.sh holds this file to
 * it: given that card's time stamp, vmc_new_ps2_image() must produce that card byte for byte (all
 * 8,650,752 of them, sha256 97d88855...). What it holds:
 *   page 0          the superblock (fields below), with ECC
 *   pages 1-15      never written: FF, spare bytes FF too
 *   cluster 8       the indirect FAT: the 32 FAT clusters 9..40, the rest FFFFFFFF
 *   clusters 9-40   the FAT, 8192 entries: [0] FFFFFFFF (the root folder's one cluster, end of chain),
 *                   [1..8134] 7FFFFFFF (free, never used), [8135..] FFFFFFFF (past alloc_end)
 *   cluster 41      the root folder: "." (mode 8427, length 2) and ".." (mode A426), both stamped
 *   everything else never written: FF, spare FF
 * A written page's 16 spare bytes: ECC, 3 bytes per 128 data bytes (vmc_ecc128), then 4 zero bytes.
 *
 * A NEW PS1 CARD (.VM1, 128 KB) is the layout of psx-spx "Memory Card Data Format", the same one
 * host/ps1_card.py's blank() builds (the card Prism's PS1 games have saved to since 2026-11-22 came
 * from it): frame 0 "MC", 15 directory frames marked free (A0, no next block), 20 broken-sector frames
 * (none), frame 63 a copy of frame 0; each frame ends in the XOR of its first 127 bytes.
 *
 * A NAME is the card file's name without .VM2 / .VM1, as the XMB shows it. vmc_name_ok() allows letters,
 * digits, space, '-', '_', '(' and ')', 1 to VMC_NAME_MAX characters, no space at either end. No name may be
 * used twice, whatever the case of its letters and whichever kind of card holds it.
 *
 * Files: a new card is written whole under "<name>.VM2.new" (a name the card lists never show), read back and
 * compared, and only then given its name. A rename is one rename of the file: the card's bytes are not touched. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef __PPU__
#include <lv2/sysfs.h>
#include <sys/file.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "vmc.h"

#define PG	512
#define RAWPG	528		/* a page and its 16 spare bytes */

static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

/* a written page: the data, its ECC, four zero bytes */
static void put_page(u8 *img, u32 n, const u8 *data)
{
	u8 *p = img + (size_t)n * RAWPG;
	int k;

	memcpy(p, data, PG);
	for (k = 0; k < 4; k++)
		vmc_ecc128(p + k * 128, p + PG + k * 3);
	memset(p + PG + 12, 0, 4);
}

/* the console clock (seconds since 1970, UTC) as a PS2 card's 8-byte time. A PS2 keeps Japan's time (UTC+9). */
void vmc_stamp(unsigned long long unix_s, u8 out[8])
{
	unsigned long long t = unix_s + 9 * 3600, days = t / 86400, rem = t % 86400;
	/* days since 1970-01-01 to a date (Howard Hinnant's civil_from_days) */
	long long z = (long long)days + 719468, era = z / 146097;
	unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	long long y = (long long)yoe + era * 400;
	unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
	unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;

	if (m <= 2)
		y++;
	out[0] = 0;
	out[1] = (u8)(rem % 60);
	out[2] = (u8)(rem / 60 % 60);
	out[3] = (u8)(rem / 3600);
	out[4] = (u8)d;
	out[5] = (u8)m;
	put16(out + 6, (u32)y);
}

/* img: VMC_PS2_BYTES. stamp: the 8-byte time the two root entries carry (vmc_stamp) */
void vmc_new_ps2_image(u8 *img, const u8 stamp[8])
{
	static const char magic[] = "Sony PS2 Memory Card Format ";	/* 28 characters */
	static const char version[] = "1.2.0.0";
	enum { CLUSTERS = 8192, ALLOC_OFFSET = 41, ALLOC_END = 8135, IFC = 8, FAT0 = 9, FAT_CL = 32, PPC = 2 };
	u8 pg[PG];
	u32 fat, c, i;

	memset(img, 0xFF, VMC_PS2_BYTES);

	/* the superblock */
	memset(pg, 0xFF, PG);
	memset(pg, 0, 0xD0);
	memcpy(pg, magic, 28);
	memcpy(pg + 28, version, 7);
	put16(pg + 0x28, PG);			/* bytes in a page */
	put16(pg + 0x2A, PPC);			/* pages in a cluster */
	put16(pg + 0x2C, 16);			/* pages in an erase block */
	put16(pg + 0x2E, 0xFF00);
	put32(pg + 0x30, CLUSTERS);
	put32(pg + 0x34, ALLOC_OFFSET);		/* the first cluster files live in */
	put32(pg + 0x38, ALLOC_END);
	put32(pg + 0x3C, 0);			/* the root folder's cluster, counted from ALLOC_OFFSET */
	put32(pg + 0x40, 1023);			/* the two blocks a real card uses while it rewrites one */
	put32(pg + 0x44, 1022);
	put32(pg + 0x50, IFC);			/* the list of indirect FAT clusters: one */
	/* 0xD0..0x14F: the list of bad blocks, none (FF, set above) */
	pg[0x150] = 0x02;			/* a PS2 card */
	pg[0x151] = 0x2B;			/* its flags */
	put32(pg + 0x154, 0x400);		/* bytes in a cluster */
	put32(pg + 0x158, 0x100);		/* FAT entries in a cluster */
	put32(pg + 0x15C, 8);			/* clusters in an erase block */
	put32(pg + 0x160, 0xFFFFFFFF);
	memset(pg + 0x164, 0, 12);
	put32(pg + 0x170, 0x1F41);
	put_page(img, 0, pg);

	/* the indirect FAT: where the FAT's clusters are */
	memset(pg, 0xFF, PG);
	for (i = 0; i < FAT_CL; i++)
		put32(pg + i * 4, FAT0 + i);
	put_page(img, IFC * PPC, pg);
	memset(pg, 0xFF, PG);
	put_page(img, IFC * PPC + 1, pg);

	/* the FAT, 128 entries a page */
	for (c = 0; c < FAT_CL * PPC; c++)
	{
		for (i = 0; i < 128; i++)
		{
			fat = c * 128 + i;
			put32(pg + i * 4, fat == 0 || fat >= ALLOC_END ? 0xFFFFFFFF : 0x7FFFFFFF);
		}
		put_page(img, FAT0 * PPC + c, pg);
	}

	/* the root folder: "." and "..", one entry a page */
	memset(pg, 0, PG);
	put16(pg, 0x8427);
	put32(pg + 4, 2);			/* entries in the root folder */
	memcpy(pg + 8, stamp, 8);
	memcpy(pg + 0x18, stamp, 8);
	pg[0x40] = '.';
	put_page(img, ALLOC_OFFSET * PPC, pg);
	memset(pg, 0, PG);
	put16(pg, 0xA426);
	memcpy(pg + 8, stamp, 8);
	memcpy(pg + 0x18, stamp, 8);
	pg[0x40] = '.';
	pg[0x41] = '.';
	put_page(img, ALLOC_OFFSET * PPC + 1, pg);
}

/* img: VMC_PS1_BYTES */
void vmc_new_ps1_image(u8 *img)
{
	int f, k;

	memset(img, 0, VMC_PS1_BYTES);
	img[0] = 'M';
	img[1] = 'C';
	for (f = 1; f < 16; f++)
	{
		u8 *p = img + f * 128;

		p[0] = 0xA0;		/* free, never used */
		p[8] = p[9] = 0xFF;	/* no next block */
	}
	for (f = 16; f < 36; f++)
	{
		u8 *p = img + f * 128;

		memset(p, 0xFF, 4);	/* no broken sector */
		p[8] = p[9] = 0xFF;
	}
	for (f = 0; f < 36; f++)
	{
		u8 *p = img + f * 128, x = 0;

		for (k = 0; k < 127; k++)
			x ^= p[k];
		p[127] = x;
	}
	memcpy(img + 63 * 128, img, 128);
}

int vmc_name_ok(const char *name, char *why, int why_n)
{
	size_t n = strlen(name), i;

	if (!n)
	{
		snprintf(why, why_n, "the name is empty");
		return -1;
	}
	if (n > VMC_NAME_MAX)
	{
		snprintf(why, why_n, "the name is longer than %d characters", VMC_NAME_MAX);
		return -2;
	}
	if (name[0] == ' ' || name[n - 1] == ' ')
	{
		snprintf(why, why_n, "the name begins or ends with a space");
		return -3;
	}
	for (i = 0; i < n; i++)
	{
		char c = name[i];

		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' || c == '(' || c == ')'))
		{
			snprintf(why, why_n, "the name holds a character a card's name cannot have");
			return -4;
		}
	}
	return 0;
}

/* is file (a directory entry's name) a card called name, in any case, of either kind? */
int vmc_same_name(const char *file, const char *name)
{
	size_t l = strlen(file), n = strlen(name);

	if (l != n + 4 || strncasecmp(file, name, n))
		return 0;
	return !strcasecmp(file + n, ".VM2") || !strcasecmp(file + n, ".VM1");
}

/* 1 = a card of that name is in dir (but_not: a file name to pass over, or NULL); -1 = dir cannot be read */
static int name_taken(const char *dir, const char *name, const char *but_not)
{
	int hit = 0;
#ifdef __PPU__
	sysFSDirent e;
	u64 rd;
	s32 fd;

	if (sysFsOpendir(dir, &fd) != 0)
		return -1;
	while (sysFsReaddir(fd, &e, &rd) == 0 && rd > 0)
		if (vmc_same_name(e.d_name, name) && !(but_not && !strcmp(e.d_name, but_not)))
			hit = 1;
	sysFsClosedir(fd);
#else
	DIR *d = opendir(dir);
	struct dirent *e;

	if (!d)
		return -1;
	while ((e = readdir(d)))
		if (vmc_same_name(e->d_name, name) && !(but_not && !strcmp(e->d_name, but_not)))
			hit = 1;
	closedir(d);
#endif
	return hit;
}

static int exists(const char *p)
{
	FILE *f = fopen(p, "rb");

	if (f)
		fclose(f);
	return f != NULL;
}

static int move_file(const char *from, const char *to)
{
#ifdef __PPU__
	return sysLv2FsRename(from, to) == 0 ? 0 : -1;
#else
	return rename(from, to);
#endif
}

static void drop_file(const char *p)
{
#ifdef __PPU__
	sysFsUnlink(p);
#else
	unlink(p);
#endif
}

/* a new empty card, dir/name.VM2 (ps1: .VM1). 0 = made; else why[] says why not and nothing of that name is left */
int vmc_new(const char *dir, const char *name, int ps1, unsigned long long unix_s, char *why, int why_n)
{
	const long len = ps1 ? VMC_PS1_BYTES : VMC_PS2_BYTES;
	char path[300], tmp[310];
	u8 *img, *back = NULL, stamp[8];
	FILE *f;
	int r = 0, t;

	if (vmc_name_ok(name, why, why_n))
		return -1;
	t = name_taken(dir, name, NULL);
	if (t)
	{
		snprintf(why, why_n, t < 0 ? "the memory card folder cannot be read" : "a card of that name is already there");
		return -2;
	}
	snprintf(path, sizeof path, "%s/%s%s", dir, name, ps1 ? ".VM1" : ".VM2");
	snprintf(tmp, sizeof tmp, "%s.new", path);
	img = (u8 *)malloc(len);
	back = (u8 *)malloc(len);
	if (!img || !back)
	{
		free(img);
		free(back);
		snprintf(why, why_n, "out of memory");
		return -3;
	}
	if (ps1)
		vmc_new_ps1_image(img);
	else
	{
		vmc_stamp(unix_s, stamp);
		vmc_new_ps2_image(img, stamp);
	}
	f = fopen(tmp, "wb");
	if (!f || fwrite(img, 1, len, f) != (size_t)len)
		r = -4;
	if (f && fclose(f) != 0)
		r = -4;
	if (!r)
	{
		/* read back whole before it gets a card's name */
		f = fopen(tmp, "rb");
		if (!f || fread(back, 1, len, f) != (size_t)len || fgetc(f) != EOF || memcmp(img, back, len))
			r = -5;
		if (f)
			fclose(f);
	}
	if (!r && exists(path))
		r = -6;		/* it appeared meanwhile */
	if (!r && move_file(tmp, path) != 0)
		r = -7;
	free(img);
	free(back);
	if (r)
	{
		drop_file(tmp);
		snprintf(why, why_n, r == -4 ? "the card file could not be written (is the hard disk full?)"
			: r == -5 ? "the new card did not read back as written" : r == -6 ? "a card of that name is already there"
			: "the new card could not be given its name");
		return r;
	}
#ifdef __PPU__
	sysFsChmod(path, 0666);
#endif
	return 0;
}

/* old_file: the card's file name as it is ("OPL.VM2"); new_name: without the ending. 0 = renamed (new_file[] gets
 * the new file name); else why[] says why not and the file is as it was */
int vmc_rename(const char *dir, const char *old_file, const char *new_name, char *new_file, int new_file_n, char *why, int why_n)
{
	size_t l = strlen(old_file);
	char from[300], to[300];
	int t;

	if (l < 5 || (strcasecmp(old_file + l - 4, ".VM2") && strcasecmp(old_file + l - 4, ".VM1")) || strchr(old_file, '/'))
	{
		snprintf(why, why_n, "that is not a card file");
		return -1;
	}
	if (vmc_name_ok(new_name, why, why_n))
		return -2;
	if (strlen(new_name) == l - 4 && !strncmp(old_file, new_name, l - 4))
	{
		snprintf(why, why_n, "that is the name it has");
		return -3;
	}
	t = name_taken(dir, new_name, old_file);	/* its own name in another case is a rename like any other */
	if (t)
	{
		snprintf(why, why_n, t < 0 ? "the memory card folder cannot be read" : "another card has that name");
		return -4;
	}
	snprintf(from, sizeof from, "%s/%s", dir, old_file);
	snprintf(new_file, new_file_n, "%s%s", new_name, old_file + l - 4);	/* the ending as it was */
	snprintf(to, sizeof to, "%s/%s", dir, new_file);
	if (!exists(from))
	{
		snprintf(why, why_n, "the card file is not there any more");
		return -5;
	}
	if (strcasecmp(old_file, new_file) && exists(to))
	{
		snprintf(why, why_n, "another card has that name");
		return -4;
	}
	if (move_file(from, to) != 0 || !exists(to))
	{
		snprintf(why, why_n, "the system did not rename the file");
		return -6;
	}
	return 0;
}
