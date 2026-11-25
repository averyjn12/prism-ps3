#ifndef PRISM_APP_LAUNCH_H
#define PRISM_APP_LAUNCH_H
#include "games.h"

/* what the app ends by starting; self[0] == 0 = just quit to the XMB */
typedef struct
{
	char self[256];
	const char *argv[9];
	int ps2;		/* hand over to the PS2 emulator (launch_ps2_now) */
	int psp;		/* v0.23: a PSP image is mounted; the player starts PSP Launcher on the XMB */
	int ppsspp;		/* v0.69: PPSSPP is being started; the game is chosen in its own list */
} spawn_t;

int launch_ps2_now(void);	/* returns only if lv2 refuses */

void launch_disarm(void);
void launch_cancel(void);	/* drop a v0.3 start request that was never run */
int  launch_prepare(int plat, const game_t *g, spawn_t *sp, char *why, int why_n);

/* v0.61: the PSP launcher a game starts with (USRDIR/psp_launcher.txt) */
#define PSPL_MINIS_	1		/* "PSP Launcher", PSPM66820 (minis) */
#define PSPL_REMASTER	2		/* "PSP Remaster Launcher", PSPC66820 */
/* v0.69: "PPSSPP PS3" (PPSSPP001), an unofficial PS3 port of the PPSSPP emulator (github.com/Calyps0man/ppsspp,
 * build 313 reviewed and installed on the E01 2026-11-25). Not Sony's emulator: its own program, its own
 * saves. It plays the games in /dev_hdd0/PSPISO and takes no game from outside: Prism can open it, the
 * game is then chosen in its own list. Never the default by itself: only when chosen. */
#define PSPL_PPSSPP	4
#define PSPL_PPSSPP_SFO	"/dev_hdd0/game/PPSSPP001/PARAM.SFO"
#define PSPL_PPSSPP_DIR	"/dev_hdd0/PSPISO/"
/* v0.72: a game on a card slot or a USB drive, for PPSSPP (launch.c pps_link): an empty file of its name in
 * PSPL_PPSSPP_DIR that webMAN is asked to redirect to the game. The empty files are listed in a file; a
 * games scan passes over them, and they are deleted once the console has restarted (the redirect is gone). */
int  pps_is_link(const char *path);	/* 1 = this path is one of those empty files, not a game */
void pps_links_clean(void);
int  psp_launchers_installed(void);			/* bits: PSPL_MINIS_ | PSPL_REMASTER */
const char *psp_game_key(const game_t *g);		/* its DISC_ID, else its file name */
int  psp_launcher_pref(const char *key);		/* key NULL = the default; 0 = none set */
int  psp_launcher_for(const game_t *g);			/* the one it will use; g NULL = the default's; 0 = none installed */
int  psp_launcher_set(const char *key, int pref);	/* key NULL = the default; pref 0 = remove */
#endif
