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
	int ppsspp;		/* v0.69: PPSSPP is being started; 1 = the game is chosen in its own list, 2 = (v0.80) it starts the game itself */
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
/* v0.80: PPSSPP build 314 and later (Prism's build of it: ppsspp_ps3/prism) starts a game it is handed, without
 * its game list (Bananafuzzbuddy, 11-26: "if we launch from the prism app or our custom xmb it launches the game outright
 * unless you change a setting in the prism app"). The hand-over: PPS_BOOT_FILE, one line "path=<the game>",
 * written just before PPSSPP is started; PPSSPP deletes it when it reads it and ignores one over 2 minutes old.
 * The setting (SETTINGS, on unless PPS_LIST_FILE is there): off = open PPSSPP's own list, the 0.69-0.79 way. */
#define PPS_BOOT_FILE	"/dev_hdd0/game/PPSSPP001/USRDIR/boot.txt"
#define PPS_LIST_FILE	"/dev_hdd0/game/PRSM00001/USRDIR/ppsspp_list.txt"
#define PPS_DIRECT_BUILD	314
int  ppsspp_build(void);		/* the installed PPSSPP's build number (its PARAM.SFO title); 0 = none or unknown */
int  ppsspp_direct_on(void);		/* the setting: 1 = start the game directly */
int  ppsspp_direct_set(int on);		/* 0 = saved */
int  ppsspp_direct(void);		/* the setting is on AND the installed PPSSPP can do it */
/* v0.80: how each PSP game starts, for Prism's XMB plugin (it starts games from webMAN's list, by path, and cannot
 * read a game's DISC_ID, by which a game's own choice is kept). Rewritten after every games scan and every launcher
 * or PPSSPP setting change. Text, one item a line:
 *   ppsspp=<0|1|2>                 0 = PPSSPP not installed, 1 = it shows its own list, 2 = it starts the game itself
 *   <m|r|p> TAB <the game's path>  PSP Launcher (minis) / PSP Remaster Launcher / PPSSPP, as Prism would start it
 * For "p" with ppsspp=2, the plugin writes PPS_BOOT_FILE ("path=<path>\n") and asks webMAN for /play.ps3?PPSSPP001.
 * A "p" game outside PSPL_PPSSPP_DIR (a card, a USB drive) needs pps_link's redirect first, which only Prism makes. */
#define PSP_TABLE	"/dev_hdd0/tmp/prism_psp_launch.txt"
void psp_table_write(void);
int  psp_launchers_installed(void);			/* bits: PSPL_MINIS_ | PSPL_REMASTER */
const char *psp_game_key(const game_t *g);		/* its DISC_ID, else its file name */
int  psp_launcher_pref(const char *key);		/* key NULL = the default; 0 = none set */
int  psp_launcher_for(const game_t *g);			/* the one it will use; g NULL = the default's; 0 = none installed */
int  psp_launcher_set(const char *key, int pref);	/* key NULL = the default; pref 0 = remove */
#endif
