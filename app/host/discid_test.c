/* v0.72: src/discid.c on the PC.
 *   bash host/discid_test.sh [image expected-serial]...   -> "discid_test: N checks, 0 failed"
 * Images are made here in memory in each layout (2048-byte ISO, raw Mode 2, raw Mode 1), with the boot
 * line written the ways real discs write it. Real images (or their first megabytes) can be named on the
 * command line with the serial each must give; they are game data and are not kept in the tree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "discid.h"

static int checks, failed;

static void check(int ok, const char *what, const char *a, const char *b)
{
	checks++;
	if (!ok)
	{
		failed++;
		printf("FAIL: %s (%s, %s)\n", what, a, b);
	}
}

typedef struct { unsigned char *p; unsigned long long n; } mem_t;

static int rd_mem(void *ctx, unsigned long long off, unsigned char *buf, unsigned len)
{
	mem_t *m = (mem_t *)ctx;

	if (off + len > m->n)
		return -1;
	memcpy(buf, m->p + off, len);
	return 0;
}

static int rd_file(void *ctx, unsigned long long off, unsigned char *buf, unsigned len)
{
	FILE *f = (FILE *)ctx;

	return fseek(f, (long)off, SEEK_SET) || fread(buf, 1, len, f) != len ? -1 : 0;
}

static void put32(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* a 40-sector image: volume at 16, root directory at 20, SYSTEM.CNF at cnf_lba holding line */
static mem_t make(int sec, int data, const char *line, int cd, int with_cnf, unsigned cnf_lba)
{
	mem_t m;
	unsigned char *s, *e;

	m.n = 40ull * sec;
	m.p = calloc(1, m.n);
	s = m.p + 16 * sec + data;
	s[0] = 1;
	memcpy(s + 1, "CD001", 5);
	if (cd)
		s[0xAC] = 8;
	else
		s[0xAC] = 0x10;
	put32(s + 158, 20);
	e = m.p + 20 * sec + data;
	e[0] = 34; e[32] = 1;				/* "." */
	e += 34;
	e[0] = 34; e[32] = 1; e[33] = 1;		/* ".." */
	e += 34;
	e[0] = 46; e[32] = 12; memcpy(e + 33, "OTHER.BIN;1", 11); put32(e + 2, 30);
	e += 46;
	if (with_cnf)
	{
		e[0] = 46; e[32] = 12; memcpy(e + 33, "SYSTEM.CNF;1", 12); put32(e + 2, cnf_lba);
	}
	if (cnf_lba < 40)
		strcpy((char *)m.p + cnf_lba * sec + data, line);
	return m;
}

int main(int argc, char **argv)
{
	static const struct { int sec, data; const char *name; } lay[3] = { { 2048, 0, "2048-byte ISO" }, { 2352, 24, "raw Mode 2" }, { 2352, 16, "raw Mode 1" } };
	static const struct { const char *line, *want; } ln[] = {
		{ "BOOT2 = cdrom0:\\SLUS_209.46;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n", "SLUS_209.46" },
		{ "BOOT = cdrom:\\slus_005.94;1\r\nTCB = 4\r\n", "SLUS_005.94" },		/* Metal Gear Solid, as its disc has it */
		{ "BOOT=cdrom:SCES_123.45;1\r\n", "SCES_123.45" },
		{ "BOOT = cdrom:\\\\SLPS_000.01\r\n", "SLPS_000.01" },
		{ "VER = 1.00\r\n", "" },								/* no boot line: a volume, no serial */
	};
	char id[16];
	int k, j, cd, raw, r, a;

	for (k = 0; k < 3; k++)
		for (j = 0; j < 5; j++)
		{
			mem_t m = make(lay[k].sec, lay[k].data, ln[j].line, k != 0, 1, 25);

			strcpy(id, "junk");
			cd = raw = -1;
			r = disc_id(&m, rd_mem, id, &cd, &raw);
			check(r == 0, "an image with a SYSTEM.CNF was not read", lay[k].name, ln[j].line);
			check(!strcmp(id, ln[j].want), "serial", id, ln[j].want);
			check(raw == (lay[k].sec == 2352), "raw 2352-byte sectors told wrongly", lay[k].name, raw ? "raw" : "not raw");
			check(cd == (k != 0), "the volume's own CD mark told wrongly", lay[k].name, cd ? "cd" : "not cd");
			free(m.p);
		}
	{
		/* no volume at all; a volume without SYSTEM.CNF; a SYSTEM.CNF past the end of the file */
		mem_t m = make(2048, 0, "", 0, 1, 25);

		memset(m.p + 16 * 2048, 0, 6);
		strcpy(id, "junk");
		check(disc_id(&m, rd_mem, id, &cd, &raw) == -1 && !id[0], "an image with no volume", id, "");
		free(m.p);
		m = make(2352, 24, "", 1, 0, 25);
		strcpy(id, "junk");
		check(disc_id(&m, rd_mem, id, &cd, &raw) == -3 && !id[0] && raw == 1, "a raw image with no SYSTEM.CNF", id, "");
		free(m.p);
		m = make(2352, 24, "", 1, 1, 4000);
		strcpy(id, "junk");
		check(disc_id(&m, rd_mem, id, &cd, &raw) == -4 && !id[0], "a SYSTEM.CNF past the end of the file", id, "");
		free(m.p);
		m.p = calloc(1, 100);
		m.n = 100;
		check(disc_id(&m, rd_mem, id, NULL, NULL) == -1 && !id[0], "a 100-byte file", id, "");
		free(m.p);
	}
	for (a = 1; a + 1 < argc; a += 2)
	{
		FILE *f = fopen(argv[a], "rb");

		check(f != NULL, "could not open", argv[a], "");
		if (!f)
			continue;
		r = disc_id(f, rd_file, id, &cd, &raw);
		printf("   %s: %d, serial '%s', %s, %s\n", strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a], r, id, raw ? "raw 2352" : "2048", cd ? "CD" : "DVD");
		check(!strcmp(id, argv[a + 1]), "a real image's serial", id, argv[a + 1]);
		fclose(f);
	}
	printf("discid_test: %d checks, %d failed\n", checks, failed);
	return failed != 0;
}
