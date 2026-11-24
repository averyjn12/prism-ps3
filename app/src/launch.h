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
#endif
