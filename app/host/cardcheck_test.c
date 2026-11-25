/* host test driver for src/cardcheck_core.c (v0.65). Run by host/cardcheck_test.py:
 *   t <card image> <reader> [from]
 * reader: good, slip, slip0, slip16, oddlba, ioerr. "from": the reader misbehaves only at sectors >= from (default 0).
 * Prints: result <code> judged <n> why <text> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cardcheck_core.h"

static FILE *img;
static const char *mode;
static uint32_t from;

static int raw(uint32_t lba, uint32_t n, uint8_t *buf)
{
	memset(buf, 0, (size_t)n * 512);
	if (fseek(img, (long)lba * 512, SEEK_SET))
		return -1;
	fread(buf, 1, (size_t)n * 512, img);	/* past the end of the file: zeros */
	return 0;
}

static int nib(const uint8_t *p, size_t j)
{
	return (j & 1) ? (p[j / 2] & 15) : (p[j / 2] >> 4);
}

/* Bananafuzzbuddy's SD-to-CF adaptor, 11-24: the real bytes, slipping by half a byte more every so often as the transfer goes
 * on. start = the nibble where the first slip happens (1024 = after the first sector, as the junk directory showed;
 * 11 = almost at once, so single-sector reads go wrong too) */
static int slipped(uint32_t lba, uint32_t n, uint8_t *buf, size_t start)
{
	size_t len = (size_t)n * 512, j;
	uint8_t *t = malloc(len + 4096);

	if (!t || raw(lba, n + 8, t))
	{
		free(t);
		return -1;
	}
	for (j = 0; j < 2 * len; j++)
	{
		size_t d = j >= start ? 1 + (j - start) / 700 : 0;
		int v = nib(t, j + d);

		if (j & 1)
			buf[j / 2] = (uint8_t)((buf[j / 2] & 0xF0) | v);
		else
			buf[j / 2] = (uint8_t)(v << 4);
	}
	free(t);
	return 0;
}

static int rd(void *ctx, uint32_t lba, uint32_t n, uint8_t *buf)
{
	uint32_t k;
	int r;

	(void)ctx;
	if (!strcmp(mode, "good") || lba + n <= from)
		return raw(lba, n, buf);
	if (!strcmp(mode, "ioerr"))
		return -1;
	if (!strcmp(mode, "slip"))
		return slipped(lba, n, buf, 1024);
	if (!strcmp(mode, "slip0"))
		return slipped(lba, n, buf, 11);
	if (!strcmp(mode, "slip16"))	/* sound for short transfers, slipping past 16 sectors: only long reads show it */
		return slipped(lba, n, buf, 16 * 1024);
	/* oddlba: the same junk in every odd sector, however it is read (a reader that mixes up addresses) */
	r = raw(lba, n, buf);
	for (k = 0; k < n && !r; k++)
		if (((lba + k) & 1) && lba + k >= from)
		{
			uint32_t i;

			for (i = 0; i < 512; i++)
				buf[k * 512 + i] ^= (uint8_t)(0x5A + i);
		}
	return r;
}

int main(int argc, char **argv)
{
	static uint8_t a[CC_BUF], b[CC_BUF];
	char why[200];
	long sz;
	int judged = -1, r;

	if (argc < 3 || !(img = fopen(argv[1], "rb")))
		return 2;
	mode = argv[2];
	from = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;
	fseek(img, 0, SEEK_END);
	sz = ftell(img);
	r = cc_card(rd, NULL, (uint32_t)(sz / 512), a, b, why, sizeof why, &judged);
	printf("result %d judged %d why %s\n", r, judged, why);
	return 0;
}
