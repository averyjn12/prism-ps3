/* v0.57: SETTINGS, "Game covers" (covers.c): look the PS1 / PS2 games up at the cover sources in
 * USRDIR/cover_sources.txt, show what each has, and download from the one the player picks into the
 * art file both Prism and the PS2 launcher read (/dev_hdd0/tmp/prism_art.dat). */
#ifndef PRISM_APP_COVERS_H
#define PRISM_APP_COVERS_H

void covers_open(void);			/* after games_scan + ui_prepare: who has a cover */
void covers_draw(void);
int  covers_pad(unsigned pressed);	/* DO_* for main, or -1 = back to SETTINGS */
void covers_find(unsigned (*pad)(void), char *msg, int n);	/* DO_COV_FIND */
int  covers_get(unsigned (*pad)(void), char *msg, int n);	/* DO_COV_GET; 1 = the art file changed */
/* v0.58: a game's Square menu (a game_t *, its platform P_PS1..P_PSP): remove its cover for good (refuse 1),
 * or let the page look it up again (0). 1 = the art file changed */
int  covers_mark(const void *g, int plat, int refuse, char *msg, int n);
/* v0.59: a cover Prism brings itself (Tiny Tees'), for id unless it has one or the player removed it.
 * plat_cv: 1 PS1 .. 4 PSP. 1 = the art file changed and is loaded again */
int  covers_builtin(const char *id, int plat_cv, const char *title, const char *rgba_path);
#endif
