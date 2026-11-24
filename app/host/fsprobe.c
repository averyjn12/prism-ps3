/* PC test tool for the app's card reader (lr_fs.c through the app's adapter).
 *   fsprobe <image> list         every game: name<TAB>sectors
 *   fsprobe <image> cat <path>   the file's bytes to stdout (lr_fs_open: whole, split, /PS2PRISM)
 *   fsprobe <image> catn <k>     game k of the list to stdout
 * The reader's own log goes to stderr. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fsport.h"

int fsport_attach_file(const char *path);

static int dump(u64 size)
{
	static u8 b[1 << 16];
	u64 off;

	for (off = 0; off < size; off += sizeof(b))
	{
		u32 n = (u32)(size - off < sizeof(b) ? size - off : sizeof(b));

		if (lr_file_read(off, b, n))
			return 3;
		fwrite(b, 1, n, stdout);
	}
	return 0;
}

int main(int argc, char **argv)
{
	u64 size = 0;
	int n, k;

	fsport_log = stderr;
	if (argc < 3 || fsport_attach_file(argv[1]))
		return 2;
	if (lr_fs_mount())
		return 1;
	if (!strcmp(argv[2], "list"))
	{
		u8 rec[128];

		n = lr_fs_list();
		if (n < 0)
			return 1;
		for (k = 0; k < n; k++)
		{
			lr_fs_cat_rec((u32)k, rec);
			printf("%s\t%u\n", lr_fs_cat_name((u32)k), rec[4] | (rec[5] << 8) | (rec[6] << 16) | ((u32)rec[7] << 24));
		}
		return 0;
	}
	if (!strcmp(argv[2], "cat") && argc > 3)
		return lr_fs_open(argv[3], &size) ? 1 : dump(size);
	if (!strcmp(argv[2], "catn") && argc > 3)
	{
		n = lr_fs_list();
		k = atoi(argv[3]);
		if (n < 0 || k >= n || lr_fs_cat_open((u32)k, &size, 1))
			return 1;
		return dump(size);
	}
	return 2;
}
