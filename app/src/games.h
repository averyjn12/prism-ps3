/* The games the app can start, per platform. */
#ifndef PRISM_APP_GAMES_H
#define PRISM_APP_GAMES_H
#include <ppu-types.h>
#include "ui.h"

#define P_PS1	0
#define P_PS2	1
#define P_PS3	2
#define P_PSP	3		/* v0.16: PSP games through the PSP launcher */
#define N_PLAT	4
#define MAX_GAMES	256
#define WHERE_DISC	11
#define WHERE_MS	12	/* v0.48: the Memory Stick slot */
#define WHERE_CF	13	/* v0.48: the CompactFlash slot */
/* a card in one of the console's own card slots (PS3 and PSP games do not start from there) */
#define WHERE_SLOT(w)	((w) == 0 || (w) == WHERE_MS || (w) == WHERE_CF)

typedef struct
{
	char name[104];		/* the file's name */
	char path[208];		/* PS1/PS3: the GameOS path webMAN mounts; PS2: the trigger path on the card */
	u64 size;
	u8 kind;		/* PS2: 0 DVD, 1 CD, 2 raw CD; PS1 CD; PS3 0 ISO, 3 folder; any: 4 the disc in the drive,
				   5 a disc shortcut whose disc is not in the drive */
	u8 where;		/* 0 /dev_sd, 1 the card read raw (PS2), 2 /dev_hdd0, 3..10 /dev_usb000..007, 11 the drive, 12 /dev_ms, 13 /dev_cf */
	u8 shortcut;		/* kept in shortcuts.txt (manage.c) */
	char id[12];		/* title ID from the image (SYSTEM.CNF), or empty */
	char shown[100];	/* what the screen shows: the art file's title, else the name without extension */
	u8 letter;		/* 0 = '#', 1..26 = A..Z */
	u8 tint;
	char init[3];
	const tex_t *cover;	/* from the art file, or NULL */
} game_t;

extern game_t games[N_PLAT][MAX_GAMES];
extern int n_games[N_PLAT];
extern int card_state;		/* 0 = card read; else why not (LR_E_*) */

void games_scan(void);
const char *where_name(int where);	/* "SD", "HDD", "USB 0" ... */

/* art.c: /dev_hdd0/tmp/prism_art.dat (bench/lr_art.py): covers and proper titles by title ID */
int  art_load(void);
int  art_load_from(const char *path);
const tex_t *art_find(const char *id, const char **title);

/* one UTF-8 key of a PARAM.SFO image (games.c) */
int sfo_get(const u8 *d, int n, const char *key, char *out, int out_n);
#endif
