#ifndef PRISM_APP_DIRLIST_H
#define PRISM_APP_DIRLIST_H
#include <ppu-types.h>

typedef struct
{
	char name[256];
	u8 dir;
	u64 size;
} dl_ent;

/* every entry of dir but "." and "..": the count (*out is malloc'd: free it), or negative */
int dir_list(const char *dir, dl_ent **out);
#endif
