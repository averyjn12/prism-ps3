/* Deleting games, and the disc shortcuts (manage.c). */
#ifndef PRISM_APP_MANAGE_H
#define PRISM_APP_MANAGE_H
#include "games.h"

/* What deleting g would remove: 0 and a line for the question ("3 files, 4.2 GB on SD card"),
 * or negative and why it cannot be deleted from here. */
int  game_del_plan(int plat, const game_t *g, char *what, int n);
unsigned long long game_del_bytes(void);	/* v0.75: bytes the last plan found */
int  game_delete(unsigned (*pad)(void), int plat, const game_t *g, char *msg, int n);	/* Circle stops it */
int  game_files(int plat, const game_t *g, char (*out)[256], int max);	/* PS1 / PS3 .iso: its files */

/* disc shortcuts: /dev_hdd0/game/PRSM00001/USRDIR/shortcuts.txt */
int  shortcut_add(int plat, const game_t *g, char *msg, int n);
int  shortcut_del(int plat, const game_t *g, char *msg, int n);

/* games.c: the disc in the drive and the saved shortcuts, added to the lists */
void scan_disc_and_shortcuts(void);
int  disc_signature(void);
int  disc_ps12(char *id, int idn, char *title, int tn);	/* v0.32: a PS1 / PS2 disc in the drive: P_PS1, P_PS2 or -1 */		/* v0.31: changes when a disc goes in or out */
void disc_event(const char *what, unsigned a, unsigned b);	/* v0.40: a line in prism_disc.log */
unsigned disc_ps_type(void);	/* v0.38: 0xFF50 PS1 CD, 0xFF60 PS2 CD, 0xFF61 PS2 DVD, or 0 */
int disc_emulated(unsigned *real, unsigned *eff);	/* v0.44: 1 = a game image stands in for the drive */
int  wm_mounted(char *what, int n);	/* 1 = webMAN has a game mounted over /dev_bdvd */
#endif
