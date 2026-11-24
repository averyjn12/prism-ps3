/* The PS3's virtual memory cards (/dev_hdd0/savedata/vmc): PS2 cards (.VM2, the PS2 card
 * filesystem in 512-byte pages, each followed by 16 spare bytes) and PS1 cards (.VM1, the raw
 * 128 KB card). Read into memory whole; saves listed with their titles and icons. */
#ifndef PRISM_APP_VMC_H
#define PRISM_APP_VMC_H
#include <ppu-types.h>
#include "ui.h"

#define VMC_DIR		"/dev_hdd0/savedata/vmc"
#define MC_CARDS	8
#define MC_SAVES	48
#define MC_VERTS	3000
#define ICON_PX		128

typedef struct
{
	char dir[33];		/* PS2: the save's folder; PS1: the save's file name */
	char title[72];		/* from icon.sys / the PS1 header, in the font's characters */
	char when[20];		/* "YYYY-MM-DD" or "" */
	u32 bytes;
	int nfiles;
	int first;		/* PS1: first block; PS2: the folder's first cluster */
	int blocks;		/* PS1: blocks used */
	int slot;		/* PS2: its entry's index in the root directory */
	int dirlen;		/* PS2: the folder's entry count, from the ROOT entry (its own "." holds 0) */
	/* the icon */
	int icon;		/* 1 = drawable */
	tex_t tex;		/* PS2: 128 x 128; PS1: 16 x 16 per frame */
	u8 *px;
	short *v;		/* PS2: nv x (x, y, z, nx, ny, nz, u, v) */
	int nv;
	float cx, cy, cz, k;
	int frames;		/* PS1: 1..3 */
	/* v0.27: the icon drawn once, at canvas size, for when it is not the chosen save (mcard.c) */
	u32 *still;		/* sw x sh, key-coloured where the icon is not */
	short sw, sh, sox, soy;	/* size; top left relative to the icon's centre, canvas pixels */
} save_t;

typedef struct
{
	char path[160];
	char name[64];		/* the file's name without .VM2 / .VM1 */
	int ps1;
	int ok;			/* 1 = formatted and readable */
	u8 *raw;
	long len;
	/* PS2 geometry */
	int page, ppc, cl, spare;
	u32 alloc_offset, alloc_end, rootdir, ifc[32];
	int free_kb;
	int n;
	save_t saves[MC_SAVES];
} card_t;

extern card_t cards[MC_CARDS];
extern int n_cards;

int  vmc_load_all(void);		/* every card in VMC_DIR; returns how many */
int  vmc_load(card_t *c, const char *path);	/* one card file (also used by the PC tests) */
void vmc_free(card_t *c);
/* a whole file of a PS2 save (into a malloc'd buffer the caller frees); length in *len */
u8  *vmc_read_file(card_t *c, const save_t *s, const char *name, u32 *len);

/* vmcw.c: change a card file. Before any change the card file as it was is written to
 * vmc_bak_dir/<file name>. 0 = done; else why[] says why not, and the file is untouched. */
extern const char *vmc_bak_dir;
int vmc_copy(card_t *from, int si, card_t *to, char *why, int why_n);
int vmc_delete(card_t *c, int si, char *why, int why_n);
/* the ECC the PS2 card keeps for each 128 bytes (checked against every written page of the
 * owner's three cards, 6327 of 6327) */
void vmc_ecc128(const u8 *chunk, u8 out[3]);

#endif
