/* v0.37: the exFAT writer (src/exfat_w.c) on an image file, for host/exw_test.sh.
 *   exw_test <image> <folder> <name> <bytes> <seed> [chunk]
 * The file's bytes are a pattern from the seed (pattern() below; exw_test.sh makes the same bytes
 * with Python to compare). Prints what exw_* said; exit 0 = the file was added. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "exfat_w.h"

static FILE *img;
static uint32_t writes_outside;

static int rd(void *ctx, uint32_t lba, uint32_t n, void *buf)
{
	(void)ctx;
	if (fseeko(img, (off_t)lba * 512, SEEK_SET) || fread(buf, 512, n, img) != n)
		return -1;
	return 0;
}

static int wr(void *ctx, uint32_t lba, uint32_t n, const void *buf)
{
	(void)ctx;
	if (fseeko(img, (off_t)lba * 512, SEEK_SET) || fwrite(buf, 512, n, img) != n)
		return -1;
	return 0;
}

/* byte i of the file: xorshift of (seed, i / 8) */
static uint8_t pattern(uint64_t seed, uint64_t i)
{
	uint64_t x = seed * 0x9E3779B97F4A7C15ull + (i >> 3) + 1;

	x ^= x >> 33;
	x *= 0xFF51AFD7ED558CCDull;
	x ^= x >> 33;
	return (uint8_t)(x >> (8 * (i & 7)));
}

static int ls_cb(void *arg, const char *name, uint64_t size, int is_dir)
{
	(void)arg;
	if (is_dir)
		printf("d %s\n", name);
	else
		printf("%llu %s\n", (unsigned long long)size, name);
	return 0;
}

int main(int argc, char **argv)
{
	exw_dev d;
	char why[200];
	uint64_t size, seed, done = 0;
	uint32_t chunk;
	uint8_t *buf;
	int r;

	if (argc == 5 && !strcmp(argv[1], "--gen"))
	{
		/* the expected bytes, to a plain file: --gen <out> <bytes> <seed> */
		FILE *o = fopen(argv[2], "wb");
		uint64_t i;

		size = strtoull(argv[3], NULL, 0);
		seed = strtoull(argv[4], NULL, 0);
		if (!o)
			return 3;
		for (i = 0; i < size; i++)
			fputc(pattern(seed, i), o);
		fclose(o);
		return 0;
	}
	if (argc >= 4 && (!strcmp(argv[1], "--mkdir") || !strcmp(argv[1], "--ls") || !strcmp(argv[1], "--cat")))
	{
		/* v0.74: --mkdir <image> <name> | --ls <image> <folder> ("size name" a line; a folder: "d name") |
		 * --cat <image> <folder> <name> (the file's bytes to stdout) */
		img = fopen(argv[2], "r+b");
		if (!img)
			return 3;
		fseeko(img, 0, SEEK_END);
		d.rd = rd;
		d.wr = wr;
		d.ctx = NULL;
		d.sectors = (uint32_t)(ftello(img) / 512);
		if (!strcmp(argv[1], "--mkdir"))
		{
			r = exw_mkdir(&d, argv[3], why, sizeof why);
			printf("mkdir %d: %s\n", r, why);
		}
		else if (!strcmp(argv[1], "--ls"))
		{
			r = exw_list(&d, argv[3], ls_cb, NULL, why, sizeof why);
			if (r)
				fprintf(stderr, "ls %d: %s\n", r, why);
		}
		else
		{
			static uint8_t big[16 << 20];
			uint32_t n = 0, k;

			memset(big, 0xA5, sizeof big);
			r = argc >= 5 ? exw_read(&d, argv[3], argv[4], big, (16 << 20) - 1024, &n, why, sizeof why) : -9;
			/* nothing past the file's own bytes may be written: the caller's buffer may end there */
			for (k = n; !r && k < n + 1024; k++)
				if (big[k] != 0xA5)
				{
					snprintf(why, sizeof why, "the read wrote past the file's size (byte %u of %u)", k, n);
					r = -8;
				}
			if (r)
				fprintf(stderr, "cat %d: %s\n", r, why);
			else
				fwrite(big, 1, n, stdout);
		}
		fclose(img);
		return r ? 1 : 0;
	}
	if (argc == 3 && !strcmp(argv[1], "--space"))
	{
		/* v0.73: --space <image>: "free total" of the exFAT volume, bytes */
		uint64_t fr = 0, tot = 0;

		img = fopen(argv[2], "rb");
		if (!img)
			return 3;
		fseeko(img, 0, SEEK_END);
		d.rd = rd;
		d.wr = wr;
		d.ctx = NULL;
		d.sectors = (uint32_t)(ftello(img) / 512);
		r = exw_space(&d, &fr, &tot, why, sizeof why);
		printf("%llu %llu %s\n", (unsigned long long)fr, (unsigned long long)tot, why);
		fclose(img);
		return r ? 1 : 0;
	}
	if (argc == 5 && (!strcmp(argv[1], "--del") || !strcmp(argv[1], "--free")))
	{
		/* v0.73: --del <image> <folder> <name>: delete it. --free <image> <folder> <unused name>: only say
		 * how much is free (exw_begin for a 1-byte file, then given up: nothing is written) */
		img = fopen(argv[2], "r+b");
		if (!img)
			return 3;
		fseeko(img, 0, SEEK_END);
		d.rd = rd;
		d.wr = wr;
		d.ctx = NULL;
		d.sectors = (uint32_t)(ftello(img) / 512);
		if (!strcmp(argv[1], "--free"))
		{
			r = exw_begin(&d, argv[3], argv[4], 1, why, sizeof why);
			printf("%llu\n", (unsigned long long)exw_free_bytes());
			exw_abort();
			fclose(img);
			return r ? 1 : 0;
		}
		r = exw_delete(&d, argv[3], argv[4], why, sizeof why);
		printf("delete %d: %s (free %llu bytes)\n", r, why, (unsigned long long)exw_free_bytes());
		fclose(img);
		return r ? 1 : 0;
	}
	if (argc < 6)
		return 2;
	img = fopen(argv[1], "r+b");
	if (!img)
		return 3;
	size = strtoull(argv[4], NULL, 0);
	seed = strtoull(argv[5], NULL, 0);
	chunk = argc > 6 ? (uint32_t)strtoul(argv[6], NULL, 0) : (1u << 20);
	fseeko(img, 0, SEEK_END);
	d.rd = rd;
	d.wr = wr;
	d.ctx = NULL;
	d.sectors = (uint32_t)(ftello(img) / 512);
	r = exw_begin(&d, argv[2], argv[3], size, why, sizeof why);
	printf("begin %d: %s (free %llu bytes)\n", r, why, (unsigned long long)exw_free_bytes());
	if (r)
		return 1;
	buf = malloc(chunk);
	while (done < size)
	{
		uint32_t n = size - done < chunk ? (uint32_t)(size - done) : chunk, i;

		for (i = 0; i < n; i++)
			buf[i] = pattern(seed, done + i);
		r = exw_write(buf, n);
		if (r)
		{
			printf("write %d at %llu\n", r, (unsigned long long)done);
			exw_abort();
			return 1;
		}
		done += n;
	}
	r = exw_finish(why, sizeof why);
	printf("finish %d: %s\n", r, why);
	(void)writes_outside;
	fclose(img);
	return r ? 1 : 0;
}
