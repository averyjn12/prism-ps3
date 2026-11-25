/* The parts of the Game covers page that need no console (v0.57, Bananafuzzbuddy 2026-11-24), tested on the PC
 * by host/covers_test.sh: serials, the source list, scaling a decoded picture to a cover, and the
 * art file (bench/lr_art.py's PRISMART1 format, read by art.c and by the PS2 launcher). */
#ifndef PRISM_APP_COVERS_CORE_H
#define PRISM_APP_COVERS_CORE_H
#include <stdio.h>
#include <stdint.h>

#define CV_COVER	128
#define CV_PIX		(CV_COVER * CV_COVER * 4)	/* one cover: R G B A, A = 0x80 */
#define CV_SECTOR	2048
#define CV_DATA_SEC	32				/* covers start here (lr_art.py) */
#define CV_REC_MAX	((CV_DATA_SEC - 1) * 16)	/* 496 records fit sectors 1 .. 31 */
#define CV_COV_MAX	511				/* 32 MiB in all: art.c ART_MAX, the stage2's LR_MENU_ART_N */
#define CV_PS2_SEEN	256				/* the PS2 launcher reads the first 256 records (CAT_MAX) */
#define CV_SRC_MAX	12

/* platforms (v0.58: PS3 and PSP too): 1 PS1, 2 PS2, 3 PS3, 4 PSP */
#define CV_PS1	1
#define CV_PS2	2
#define CV_PS3	3
#define CV_PSP	4
#define CV_REFUSED	0x80	/* in a record's platform byte: the player removed this cover; never look again */

typedef struct
{
	char name[64];
	uint8_t plats;		/* bit p set: platform p (1 << CV_PS1 ...) */
	uint8_t local;		/* 0 a web address; 1 a file on this console; 2 (v0.58) the game's own ICON0.PNG */
	char host[64];		/* web: the server */
	char path[208];		/* web: the path on it; local: the whole path; {ID} / {DASH} in either */
} cv_src;

typedef struct
{
	char id[12];		/* SLUS_212.43, or BLUS30386 / ULUS10041 */
	char title[108];	/* ASCII, may be empty */
	uint8_t plat;		/* CV_PS1 .. CV_PSP, 0 not known (taken as PS2: lr_art.py only ever wrote PS2) */
	uint8_t refused;	/* the player removed its cover (CV_REFUSED) */
	const uint8_t *px;	/* CV_PIX bytes, or NULL: no cover */
} cv_entry;

/* "SLUS_212.43" (PS1 / PS2: 4 letters _ 3 digits . 2 digits) or "BLUS30386" (PS3 / PSP: 4 letters 5 digits) */
int  cv_is_id(const char *id);
void cv_dash(const char *id, char *out);		/* SLUS_212.43 -> SLUS-21243, BLUS30386 -> BLUS-30386 (12 bytes) */
int  cv_expand(const char *tpl, const char *id, char *out, int n);	/* 0, or -1: does not fit / bad ID */
int  cv_sources_parse(const char *text, cv_src *out, int max);	/* how many */
/* a decoded picture (4 bytes a pixel, A R G B, pitch bytes a row) to one cover, by area average: a box
 * cover fills the square (the jacket is square; a box is squeezed, as lr_art.py does it) */
int  cv_scale(const uint8_t *argb, int w, int h, int pitch, uint8_t *out);
/* v0.58: the same, but the picture keeps its shape, centred, the rest filled with its own average colour
 * darkened: for a game's own icon (PSP 144 x 80, PS3 320 x 176), which squeezed would look wrong */
int  cv_fit(const uint8_t *argb, int w, int h, int pitch, uint8_t *out);
/* v0.59: either, by the picture's shape: wider than 1.15 : 1 is an icon (cv_fit), else a box (cv_scale) */
int  cv_cover(const uint8_t *argb, int w, int h, int pitch, uint8_t *out);
/* v0.59: the folder a source needs on this console before it is offered (a cover pack's), or 0: offered
 * always (a web address, or a picture of the game's own: game:) */
int  cv_src_folder(const cv_src *s, char *out, int n);
/* v0.59: the k-th name a picture beside the game may have (webMAN's way: the game's file name with
 * .png / .jpg / .PNG / .JPG for its extension). 0, or -1: no k-th (or the game is not a file on the
 * console's own file system) */
int  cv_beside(const char *game_path, int k, char *out, int n);
/* the records of an art file held in memory; px points into buf. How many, or < 0: not an art file */
int  cv_art_parse(const uint8_t *buf, long len, cv_entry *out, int max);
/* writes an art file: PS2 (and not-known) records first so the launcher's 256 are PS2 games, then
 * PS1; at most CV_REC_MAX records and CV_COV_MAX covers. Returns the covers written, < 0 on error */
int  cv_art_write(const cv_entry *e, int n, FILE *f);
#endif
