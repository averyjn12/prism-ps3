/* The one-time setup PS2 games from the SD card need (setup.c). */
#ifndef PRISM_APP_SETUP_H
#define PRISM_APP_SETUP_H
#include <stdint.h>

enum { SU_NONE, SU_APP, SU_BENCH, SU_CONFLICT };

typedef struct
{
	int state;		/* how /dev_hdd0/onxmb.bat loads the helper: SU_* */
	int stage2;		/* /dev_hdd0/tmp/prism_stage2.bin: 1 = the one shipped, 0 = another, -1 = missing */
	int carrier;		/* /dev_hdd0/PS2ISO/Prism Launcher.iso: the same */
	int pending;		/* our lines are written but the XMB has not run them yet */
	int shipped;		/* the app carries the setup files (USRDIR/setup) */
	char line[120];		/* SU_CONFLICT: the line that loads another stage2 */
} setup_t;

extern setup_t su;

void setup_read(void);
int  setup_install(char *msg, int n);	/* 0 = done; msg says what happened either way */
int  setup_remove(char *msg, int n);
int  ps2_ready(char *why, int n);	/* 1 = a PS2 start from the card can work right now */
void tinytees_install(void);		/* once per install: Tiny Tees in /dev_hdd0/PS2ISO */
int  tinytees_cover(void);		/* v0.59: every start, after art_load: its cover, if it has none */
int  wm_shortcuts_add(char *msg, int n);	/* v0.55: the card's PS2 games in webMAN's list (SETTINGS) */
/* v0.56: one card PS2 game (a game_t *) in / out of webMAN's list, from its Square menu */
int  wm_shortcut_has(const void *g);
int  wm_shortcut_one(const void *g, int add, char *msg, int n);
int  wm_shortcut_many(const void *list, int cnt, int add, char *msg, int n);	/* v0.78: the marked games (game_t list[cnt]) */
/* v0.28: SETTINGS "Start Prism with the console" (a block in /dev_hdd0/autoexec.bat) */
int  autostart_on(void);
int  autostart_set(int on, char *msg, int n);
void autostart_started(void);
uint64_t wall_s(void);			/* v0.32: the console clock, seconds */
extern uint64_t t_main_s, t_intro_s;	/* v0.32: when main() began and the intro reached t = 0 */		/* v0.29: at start: spend the marker, rewrite old lines, unmount the boot disc */
#endif
