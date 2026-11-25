/* The art file the PS2 launcher already uses (bench/lr_art.py): sector 0 text "PRISMART1",
 * count=, w=128, h=128, data=<first cover sector>; from sector 1 one 128-byte record per title
 * ID ([0..11] ID, [16..17] cover slot little-endian, 0xFFFF none, [20..] proper title); covers
 * are 128 x 128 x R G B A (A 0x80) from sector data. Read whole into memory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "games.h"

#define ART_FILE	"/dev_hdd0/tmp/prism_art.dat"
#define ART_MAX		(32 * 1024 * 1024)
#define COVER		128

static u8 *art;
static long art_len;
static int n_rec, data_sec;
static tex_t *texs;

static int num_after(const char *t, const char *key)
{
	const char *p = strstr(t, key);

	return p ? atoi(p + strlen(key)) : -1;
}

/* v0.57: the file as loaded, for the Game covers page to rebuild it from (covers.c) */
const u8 *art_bytes(long *len)
{
	*len = art ? art_len : 0;
	return art;
}

int art_load_from(const char *path)
{
	FILE *f;
	char head[2049];
	int i;

	/* v0.57: loaded again after the covers page rewrote it. Every game's cover pointer is into the old
	 * buffer: the caller re-runs ui_prepare() (main.c rescan) before anything is drawn */
	free(texs);
	free(art);
	texs = NULL;
	art = NULL;
	n_rec = 0;
	f = fopen(path, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	art_len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (art_len < 4096 || art_len > ART_MAX)
	{
		fclose(f);
		return -2;
	}
	art = (u8 *)malloc(art_len);
	if (!art || fread(art, 1, art_len, f) != (size_t)art_len)
	{
		fclose(f);
		return -3;
	}
	fclose(f);
	memcpy(head, art, 2048);
	head[2048] = 0;
	n_rec = num_after(head, "\ncount=");
	data_sec = num_after(head, "\ndata=");
	if (strncmp(head, "PRISMART1\n", 10) != 0 || num_after(head, "\nw=") != COVER || num_after(head, "\nh=") != COVER
		|| n_rec <= 0 || data_sec < 1 || 2048L + 128L * n_rec > art_len)
	{
		free(art);	/* v0.57: nothing half-loaded stays */
		art = NULL;
		n_rec = 0;
		return -4;
	}
	texs = (tex_t *)calloc(n_rec, sizeof(tex_t));
	for (i = 0; texs && i < n_rec; i++)
	{
		const u8 *r = art + 2048 + 128 * i;
		int slot = r[16] | (r[17] << 8);
		long off = (long)data_sec * 2048 + (long)slot * COVER * COVER * 4;

		if (slot != 0xFFFF && off + COVER * COVER * 4 <= art_len)
		{
			texs[i].w = COVER;
			texs[i].h = COVER;
			texs[i].px = art + off;
		}
	}
	return n_rec;
}

const tex_t *art_find(const char *id, const char **title)
{
	int i;

	if (title)
		*title = NULL;
	if (!art || !id[0])
		return NULL;
	for (i = 0; i < n_rec; i++)
	{
		const char *r = (const char *)art + 2048 + 128 * i;

		if (strncmp(r, id, 11) == 0)
		{
			if (title && r[20])
				*title = r + 20;
			return texs && texs[i].px ? &texs[i] : NULL;
		}
	}
	return NULL;
}

/* v0.58: a record whose cover the player removed: byte 18 has 0x80 (covers_core.h CV_REFUSED) */
int art_refused(const char *id)
{
	int i;

	if (!art || !id[0])
		return 0;
	for (i = 0; i < n_rec; i++)
	{
		const u8 *r = art + 2048 + 128 * i;

		if (strncmp((const char *)r, id, 11) == 0)
			return (r[18] & 0x80) != 0;
	}
	return 0;
}

int art_load(void)
{
	return art_load_from(ART_FILE);
}
