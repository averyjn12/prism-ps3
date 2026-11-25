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
} spawn_t;

int launch_ps2_now(void);	/* returns only if lv2 refuses */

void launch_disarm(void);
void launch_cancel(void);	/* drop a v0.3 start request that was never run */
int  launch_prepare(int plat, const game_t *g, spawn_t *sp, char *why, int why_n);

/* v0.61: the PSP launcher a game starts with (USRDIR/psp_launcher.txt) */
#define PSPL_MINIS_	1		/* "PSP Launcher", PSPM66820 (minis) */
#define PSPL_REMASTER	2		/* "PSP Remaster Launcher", PSPC66820 */
int  psp_launchers_installed(void);			/* bits: PSPL_MINIS_ | PSPL_REMASTER */
const char *psp_game_key(const game_t *g);		/* its DISC_ID, else its file name */
int  psp_launcher_pref(const char *key);		/* key NULL = the default; 0 = none set */
int  psp_launcher_for(const game_t *g);			/* the one it will use; g NULL = the default's; 0 = none installed */
int  psp_launcher_set(const char *key, int pref);	/* key NULL = the default; pref 0 = remove */
#endif
