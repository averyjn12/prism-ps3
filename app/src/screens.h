/* The launcher's screens on the PS3 side (ported from launcher/ui/main.c and mcard.h). */
#ifndef PRISM_APP_SCREENS_H
#define PRISM_APP_SCREENS_H

#define PAD_UP		0x0001
#define PAD_DOWN	0x0002
#define PAD_LEFT	0x0004
#define PAD_RIGHT	0x0008
#define PAD_CROSS	0x0010
#define PAD_CIRCLE	0x0020
#define PAD_SQUARE	0x0040
#define PAD_TRIANGLE	0x0080
#define PAD_L1		0x0100
#define PAD_R1		0x0200
#define PAD_SELECT	0x0400
#define PAD_START	0x0800
#define PAD_L2		0x1000
#define PAD_R2		0x2000

enum { SCR_HOME, SCR_GAMES, SCR_SEARCH, SCR_MC, SCR_SETTINGS };

/* what the pad asked the app to do (main.c acts on it) */
enum { DO_NOTHING, DO_START, DO_RESCAN, DO_QUIT, DO_PROBE, DO_RIP,
	/* v0.15 */
	DO_SETUP, DO_UNSETUP, DO_RECHECK, DO_DELETE_ASK, DO_DELETE, DO_COPYGAME, DO_SC_ADD, DO_SC_DEL,
	DO_AUTOSTART,
	/* v0.49 */
	DO_WMFIX,
	/* v0.55 */
	DO_WMSC,
	/* v0.56 */
	DO_WM_ADD, DO_WM_DEL,
	/* v0.57: SETTINGS, Game covers (covers.c) */
	DO_COV_FIND, DO_COV_GET,
	/* v0.58: a game's Square menu */
	DO_COV_DEL, DO_COV_ALLOW,
	/* v0.61: a PSP game's own launcher */
	DO_PSPL_REM, DO_PSPL_MIN,
	/* v0.67: copy the marked games (rip.c copy_games) */
	DO_COPYMARKED,
	/* v0.69: a PSP game opens in PPSSPP (launch.h PSPL_PPSSPP) */
	DO_PSPL_PPS,
	DO_DELMARKED_ASK, DO_DELMARKED,	/* v0.75: every marked game, after the same two questions */
	DO_WM_ADD_MARKED, DO_WM_DEL_MARKED };	/* v0.78: the marked card games in / out of webMAN's list */

extern unsigned frame;		/* 1/60 s since the app started */
extern unsigned long long ui_now_s;	/* v0.67: the console clock, seconds (main.c keeps it; for "2 hours ago") */
extern int screen, plat;
extern u64 (*ui_clock)(void);
extern u64 ui_t_bgcopy, ui_t_backdrop;

void ui_prepare(void);		/* after games_scan(): titles, covers, letters, A-Z */
void ui_draw(void);
int  ui_pad(unsigned pressed, unsigned held);	/* returns DO_* */
const void *ui_chosen(void);	/* the game_t to start */
unsigned ui_nav_sig(void);	/* v0.72: changes when the pad moved or changed something (for the menu sounds) */
unsigned mc_nav_sig(void);	/* mcard.c */
unsigned settings_nav_sig(void);	/* settings.c, the help pages included */
unsigned help_nav_sig(void);	/* help.c */
struct game_s;
void ui_ask_delete_many(int n, const char *what);	/* v0.75 */
int  ui_marked(void *out, int max);	/* v0.67: copies of the marked games of this list (game_t out[max]); the count */
void ui_say(const char *s);
void progress(const void *g, const char *head, const char *title, float done, const char *what);
void progress_draw(const void *g, const char *head, const char *title, float done, const char *what);	/* 1.00: busy.c */
extern int ui_names_clean;	/* 1.00: Settings "Game names: clean" (setup.c names_clean); ui_prepare uses it */
int name_without_id(const char *in, char *out, int n);

void ui_ask_delete(const char *what);	/* main.c, after game_del_plan(): the first of the two questions */
void ui_open_settings(void);
void ui_home_go(int tile);		/* 0 PS1, 1 PS2, 2 PSP, 3 PS3, 4 MEMORY CARD, 5 SETTINGS */

/* settings.c */
void settings_open(void);
void settings_draw(void);
int  settings_pad(unsigned pressed);		/* DO_* for main, or -1 = back to HOME */
void settings_autostart_changed(void);	/* v0.28: re-read the switch after main flipped it */

/* the storage devices and their free space, for SETTINGS (filled by dev_space_scan, check.c) */
#define DEV_SPACE_N	12
typedef struct
{
	const char *name;
	unsigned long long free;
} dev_space_t;
extern dev_space_t dev_space[DEV_SPACE_N];
extern int n_dev_space;
void dev_space_scan(void);

/* mcard.c */
void mc_open(void);
void mc_draw(void);
int  mc_pad(unsigned pressed);	/* 1 = back to HOME */
void mc_step(void);

#endif
