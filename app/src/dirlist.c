/* A folder's entries with their type and size, read in batches from the folder itself
 * (sysFsGetDirectoryEntries), and the folder closed again before the caller does anything.
 *
 * Why (v0.16): on the console's SD slot a stat of a file in a 400-file folder costs ~0.1 s (the
 * FAT driver scans the folder for the name), and v0.15 made two per file to plan a delete: about
 * a minute and a half of a still screen for one PS3 game (owner, 2026-11-22: "it looks like my
 * console froze again"). Reading every entry first also means a delete never changes a folder
 * while it is being listed. */
#include <stdlib.h>
#include <string.h>
#include <lv2/sysfs.h>
#include "dirlist.h"

int dir_list(const char *dir, dl_ent **out)
{
	sysFSDirectoryEntry *b;
	dl_ent *v = NULL;
	int n = 0, cap = 0;
	s32 fd;
	u32 got = 0, i;

	*out = NULL;
	if (sysFsOpendir(dir, &fd) != 0)
		return -1;
	b = (sysFSDirectoryEntry *)malloc(32 * sizeof *b);
	if (!b)
	{
		sysFsClosedir(fd);
		return -2;
	}
	while (sysFsGetDirectoryEntries(fd, b, 32 * sizeof *b, &got) == 0 && got > 0)
	{
		for (i = 0; i < got; i++)
		{
			const char *nm = b[i].entry_name.d_name;

			if (!strcmp(nm, ".") || !strcmp(nm, ".."))
				continue;
			if (n == cap)
			{
				dl_ent *w = (dl_ent *)realloc(v, (cap ? cap * 2 : 64) * sizeof *v);

				if (!w)
					break;
				v = w;
				cap = cap ? cap * 2 : 64;
			}
			strncpy(v[n].name, nm, sizeof v[n].name - 1);
			v[n].name[sizeof v[n].name - 1] = 0;
			v[n].dir = b[i].entry_name.d_type == 1 || (b[i].attribute.st_mode & 0170000) == 0040000;
			v[n].size = v[n].dir ? 0 : b[i].attribute.st_size;
			n++;
		}
	}
	free(b);
	sysFsClosedir(fd);
	*out = v;
	return n;
}
