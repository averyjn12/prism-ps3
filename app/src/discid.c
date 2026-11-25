/* v0.72: a PS1 / PS2 disc image's serial, from the boot file its SYSTEM.CNF names. Out of games.c so the
 * PC can test it (host/discid_test.c), after two games on the console had no cover and could get none
 * (Bananafuzzbuddy, 11-25):
 *  - Metal Gear Solid (PS1): its SYSTEM.CNF says "BOOT = cdrom:\slus_005.94;1", in small letters. The
 *    serial was kept as written, and nothing knows "slus_005.94". Serials are now made capital.
 *  - Crazy Taxi (PS2): a CD game kept as raw 2352-byte sectors. Only PS1 images were tried that way; a
 *    PS2 one was read as 2048-byte sectors, found no volume, and was left without a serial. */
#include <string.h>
#include "discid.h"

static unsigned le32(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* the boot file named in a SYSTEM.CNF ("BOOT2 = cdrom0:\SLUS_209.46;1", PS1 "cdrom:\...") */
static void cnf_id(char *out, char *b, int n)
{
	char *p;
	int i;

	b[n - 1] = 0;
	for (p = b; *p; p++)
	{
		if (!strncmp(p, "cdrom0:", 7) || !strncmp(p, "cdrom:", 6))
		{
			p += (p[5] == '0') ? 7 : 6;
			while (*p == '\\' || *p == '/')
				p++;
			for (i = 0; i < 11 && p[i] > ' ' && p[i] != ';'; i++)
				out[i] = (p[i] >= 'a' && p[i] <= 'z') ? (char)(p[i] - 32) : p[i];
			out[i] = 0;
			return;
		}
	}
}

/* one layout: sec bytes a sector, the 2048 user bytes at +data. -1 = no ISO9660 volume read this way */
static int iso_id(void *ctx, disc_rd rd, int sec, int data, char *out, int *is_cd)
{
	unsigned char b[2048];
	unsigned i;

	out[0] = 0;
	if (rd(ctx, 16ull * sec + data, b, 2048) || b[0] != 1 || memcmp(b + 1, "CD001", 5))
		return -1;
	if (is_cd)
		*is_cd = (b[0xAA] == 0 && b[0xAB] == 0 && b[0xAC] == 8 && b[0xAD] == 0);	/* Cobra's rule, as the stage2 */
	if (rd(ctx, (unsigned long long)le32(b + 158) * sec + data, b, 2048))
		return -2;
	for (i = 0; i + 44 < 2048 && b[i]; i += b[i])
	{
		if (b[i + 32] >= 10 && !memcmp(b + i + 33, "SYSTEM.CNF", 10))
			break;
	}
	if (i + 44 >= 2048 || !b[i])
		return -3;
	if (rd(ctx, (unsigned long long)le32(b + i + 2) * sec + data, b, 2048))
		return -4;
	cnf_id(out, (char *)b, 512);
	return 0;
}

int disc_id(void *ctx, disc_rd rd, char *out, int *is_cd, int *raw)
{
	/* a 2048-byte ISO, or raw 2352-byte sectors: Mode 2 user data at +24, Mode 1 at +16 */
	static const int fmt[3][2] = { { 2048, 0 }, { 2352, 24 }, { 2352, 16 } };
	int k, r = -1;

	out[0] = 0;
	if (is_cd)
		*is_cd = 0;
	if (raw)
		*raw = 0;
	for (k = 0; k < 3; k++)
	{
		r = iso_id(ctx, rd, fmt[k][0], fmt[k][1], out, is_cd);
		if (r != -1)
		{
			if (raw)
				*raw = fmt[k][0] == 2352;
			return r;
		}
	}
	return -1;
}
