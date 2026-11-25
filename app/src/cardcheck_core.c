/* v0.65: does a card slot give back the card's real bytes? What and why: cardcheck_core.h. Read-only. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "cardcheck_core.h"

#define MAX_VOLS	4

typedef struct
{
	cc_read rd;
	void *ctx;
	uint32_t total;
	uint8_t *a, *b;
	char *why;
	int wn, bad, ioerr, judged;
} cc_t;

static uint32_t le16(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
	return le16(p) | (le16(p + 2) << 16);
}

static uint64_t le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/* the first failure names itself; a BAD outranks a read error */
static void note(cc_t *c, int bad, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void note(cc_t *c, int bad, const char *fmt, ...)
{
	va_list ap;

	if (c->bad || (!bad && c->ioerr))
		return;
	va_start(ap, fmt);
	vsnprintf(c->why, (size_t)c->wn, fmt, ap);
	va_end(ap);
	if (bad)
		c->bad = 1;
	else
		c->ioerr = 1;
}

static int rd(cc_t *c, uint32_t lba, uint32_t n, uint8_t *buf)
{
	if (lba >= c->total || n > c->total - lba)
		return -1;
	if (c->rd(c->ctx, lba, n, buf))
	{
		note(c, 0, "a read of %u sector(s) at %u failed", n, lba);
		return -1;
	}
	return 0;
}

/* the same sectors three ways: one transfer, one sector at a time, and a second transfer starting halfway in */
static void same3(cc_t *c, uint32_t lba, uint32_t n, const char *what)
{
	uint32_t k, h, nz = 0;

	if (c->bad || lba >= c->total)
		return;
	if (n > c->total - lba)
		n = c->total - lba;
	if (n > CC_SPAN)
		n = CC_SPAN;
	if (n < 2)
		return;
	if (rd(c, lba, n, c->a))
		return;
	for (k = 0; k < n; k++)
		if (rd(c, lba + k, 1, c->b + k * 512))
			return;
	for (k = 0; k < n; k++)
		if (memcmp(c->a + k * 512, c->b + k * 512, 512))
		{
			note(c, 1, "%s: sector %u read in a %u-sector transfer differs from the same sector read alone", what, lba + k, n);
			return;
		}
	h = n / 2;
	if (rd(c, lba + h, n - h, c->a))
		return;
	for (k = 0; k < n - h; k++)
		if (memcmp(c->a + k * 512, c->b + (h + k) * 512, 512))
		{
			note(c, 1, "%s: sector %u differs when the transfer starts at sector %u", what, lba + h + k, lba + h);
			return;
		}
	/* a slip shows only where there are bytes to slip: count the test if the sectors after the first hold data */
	for (k = 512; k < n * 512; k++)
		nz += c->b[k] != 0;
	if (nz >= 256)
		c->judged++;
}

uint32_t cc_exfat_sum(const uint8_t *s)
{
	uint32_t sum = 0, i;

	for (i = 0; i < 11 * 512; i++)
	{
		if (i == 106 || i == 107 || i == 112)
			continue;
		sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + s[i];
	}
	return sum;
}

static int is_exfat(const uint8_t *s)
{
	return memcmp(s + 3, "EXFAT   ", 8) == 0;
}

static int is_fat32(const uint8_t *s)
{
	return (s[0] == 0xEB || s[0] == 0xE9) && le16(s + 11) == 512 && s[13] && !(s[13] & (s[13] - 1)) && s[16]
		&& le16(s + 17) == 0 && le16(s + 22) == 0 && le32(s + 36);
}

static void exfat_vol(cc_t *c, uint32_t p)
{
	uint32_t sum, i, fat, heap, shift, root;

	if (rd(c, p, 12, c->a))
		return;
	sum = cc_exfat_sum(c->a);
	for (i = 0; i < 128; i++)
		if (le32(c->a + 11 * 512 + 4 * i) != sum)
		{
			note(c, 1, "exFAT volume at %u: its boot checksum does not match its boot sectors", p);
			return;
		}
	c->judged++;
	/* the backup boot region: the same, but for the two fields the main copy keeps current (spec 3.1.13, 3.1.16) */
	if (rd(c, p + 12, 12, c->b))
		return;
	for (i = 0; i < 12 * 512; i++)
		if (c->a[i] != c->b[i] && i != 106 && i != 107 && i != 112)
		{
			note(c, 1, "exFAT volume at %u: byte %u of its boot region differs from the backup copy", p, i);
			return;
		}
	c->judged++;
	fat = p + le32(c->a + 80);
	heap = p + le32(c->a + 88);
	shift = c->a[109];
	root = le32(c->a + 96);
	same3(c, fat, CC_SPAN, "exFAT FAT");
	if (shift < 16 && root >= 2)
		same3(c, heap + ((root - 2) << shift), CC_SPAN, "exFAT root directory");
}

static void fat32_vol(cc_t *c, uint32_t p)
{
	uint32_t fsi, bk, fat, nf, fatsz, ext, spc, root, data, m, i;

	if (rd(c, p, 12, c->a))
		return;
	fsi = le16(c->a + 48);
	bk = le16(c->a + 50);
	fat = p + le16(c->a + 14);
	nf = c->a[16];
	fatsz = le32(c->a + 36);
	ext = le16(c->a + 40);
	spc = c->a[13];
	root = le32(c->a + 44);
	if (fsi >= 1 && fsi < 12)
	{
		const uint8_t *f = c->a + fsi * 512;

		if (le32(f) != 0x41615252 || le32(f + 484) != 0x61417272 || le32(f + 508) != 0xAA550000)
		{
			note(c, 1, "FAT32 volume at %u: its FSInfo sector (%u) lost its signatures", p, fsi);
			return;
		}
		c->judged++;
	}
	if (bk >= 1 && bk < 12 && bk != fsi)
	{
		/* the backup boot sector; bytes 64-89 (drive number, serial, label) may be kept apart by some tools */
		const uint8_t *s = c->a, *t = c->a + bk * 512;

		for (i = 0; i < 512; i++)
			if (s[i] != t[i] && (i < 64 || i > 89))
			{
				note(c, 1, "FAT32 volume at %u: byte %u of its boot sector differs from the backup at sector %u", p, i, bk);
				return;
			}
		c->judged++;
	}
	/* the FAT against its mirror, each in one transfer (entries 0 and 1 hold flags a system may set in one copy only) */
	if (nf >= 2 && !(ext & 0x80) && fatsz)
	{
		m = fatsz < CC_SPAN ? fatsz : CC_SPAN;
		if (rd(c, fat, m, c->a) || rd(c, fat + fatsz, m, c->b))
			return;
		for (i = 8; i < m * 512; i++)
			if (c->a[i] != c->b[i])
			{
				note(c, 1, "FAT32 volume at %u: byte %u of the FAT differs from its mirror", p, i);
				return;
			}
		c->judged++;
	}
	same3(c, fat, CC_SPAN, "FAT32 FAT");
	data = fat + nf * fatsz;
	if (root >= 2 && spc)
		same3(c, data + (root - 2) * spc, CC_SPAN, "FAT32 root directory");
}

int cc_card(cc_read rdf, void *ctx, uint32_t total, uint8_t *a, uint8_t *b, char *why, int wn, int *judged)
{
	cc_t c;
	uint32_t vols[MAX_VOLS], nv = 0, i;

	memset(&c, 0, sizeof c);
	c.rd = rdf;
	c.ctx = ctx;
	c.total = total;
	c.a = a;
	c.b = b;
	c.why = why;
	c.wn = wn;
	why[0] = 0;
	if (rd(&c, 0, 1, a) == 0 && a[510] == 0x55 && a[511] == 0xAA)
	{
		if (is_exfat(a) || is_fat32(a))
			vols[nv++] = 0;
		else
			for (i = 0; i < 4; i++)
			{
				const uint8_t *e = a + 446 + 16 * i;

				if (e[4] == 0xEE && !nv)
				{
					/* GPT: the first four entries' start sectors */
					uint64_t el;
					int k;

					if (rd(&c, 1, 1, b) || memcmp(b, "EFI PART", 8))
						break;
					el = le64(b + 72);
					if (el >> 32 || rd(&c, (uint32_t)el, 1, b))
						break;
					for (k = 0; k < 4 && nv < MAX_VOLS; k++)
					{
						uint64_t first = le64(b + 128 * k + 32);

						if (first && !(first >> 32) && first < total)
							vols[nv++] = (uint32_t)first;
					}
					break;
				}
				if (e[4] && e[4] != 0xEE && le32(e + 8) && le32(e + 8) < total && nv < MAX_VOLS)
					vols[nv++] = le32(e + 8);
			}
	}
	same3(&c, 0, CC_SPAN, "the card's first sectors");
	if (!nv)	/* no table read (or none there): the place a first partition usually starts holds bytes to judge by */
		same3(&c, 2048, CC_SPAN, "sector 2048");
	for (i = 0; i < nv && !c.bad; i++)
	{
		if (rd(&c, vols[i], 1, a))
			continue;
		if (is_exfat(a))
			exfat_vol(&c, vols[i]);
		else if (is_fat32(a))
			fat32_vol(&c, vols[i]);
		else
			same3(&c, vols[i], CC_SPAN, "a volume Prism does not read");
	}
	if (judged)
		*judged = c.judged;
	if (c.bad)
		return CC_BAD;
	if (c.ioerr)
		return CC_IOERR;
	if (!c.judged)
	{
		snprintf(why, (size_t)wn, "nothing on the card to judge the slot by");
		return CC_THIN;
	}
	snprintf(why, (size_t)wn, "%d test(s) agreed", c.judged);
	return CC_OK;
}
