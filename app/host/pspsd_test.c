/* src/pspsd_core.c on the PC (host/pspsd_test.sh drives it):
 *   t header <name> <size> <out>                       the fake volume's 0xD000-byte header
 *   t args <device hex> <hdr path> <out> <lba:cnt>...  the .ntfs[PSPISO] bytes
 *   t ps3args <device hex> <out> <lba:cnt>...          v0.82: the .ntfs[PS3ISO] bytes (host/ps3sd_test.py) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pspsd_core.h"

int main(int argc, char **argv)
{
	/* twice webMAN's 64 KB: so only fi_args' own limit can refuse a .ntfs file that is too big */
	static uint8_t buf[2 * (FI_ARGS_MAX > FI_HDR_SIZE ? FI_ARGS_MAX : FI_HDR_SIZE)];
	FILE *f;

	if (argc == 5 && !strcmp(argv[1], "header"))
	{
		if (fi_header(argv[2], strtoull(argv[3], NULL, 10), buf))
			return 2;
		f = fopen(argv[4], "wb");
		fwrite(buf, 1, FI_HDR_SIZE, f);
		fclose(f);
		return 0;
	}
	if (argc >= 6 && !strcmp(argv[1], "args"))
	{
		static uint32_t lba[9000], cnt[9000];
		int n = 0, i, r;

		for (i = 5; i < argc && n < 9000; i++, n++)
			if (sscanf(argv[i], "%u:%u", &lba[n], &cnt[n]) != 2)
				return 3;
		r = fi_args(strtoull(argv[2], NULL, 16), argv[3], lba, cnt, n, buf, sizeof buf);
		if (r < 0)
			return 4;
		f = fopen(argv[4], "wb");
		fwrite(buf, 1, r, f);
		fclose(f);
		return 0;
	}
	if (argc >= 5 && !strcmp(argv[1], "ps3args"))
	{
		static uint32_t lba[9000], cnt[9000];
		int n = 0, i, r;

		for (i = 4; i < argc && n < 9000; i++, n++)
			if (sscanf(argv[i], "%u:%u", &lba[n], &cnt[n]) != 2)
				return 3;
		r = ps3_args(strtoull(argv[2], NULL, 16), lba, cnt, n, buf, sizeof buf);
		if (r < 0)
			return 4;
		f = fopen(argv[3], "wb");
		fwrite(buf, 1, r, f);
		fclose(f);
		return 0;
	}
	return 9;
}
