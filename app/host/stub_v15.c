/* PC stand-ins for the console-only parts v0.15 added (check.c, setup.c): the renderer and the
 * bench set chk / su to the state they want to draw. */
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "check.h"
#include "setup.h"
#include "screens.h"

check_t chk;
setup_t su;

const char *model_text(int c)
{
	return c == 2 ? "CECHC / CECHE, COK-002" : c == 1 ? "CECHA / CECHB, COK-001" : "unknown";
}

int model_family(int c)
{
	return c == 2 || c == 3 ? 2 : c == 1 ? 1 : 0;
}

int ps2_ready(char *why, int n)
{
	if (!chk.ps2_ok)
		snprintf(why, n, "PS2 games from the card do not work on this console: %s", chk.why);
	else if (su.state == SU_NONE)
		snprintf(why, n, "PS2 play needs a one-time setup: SETTINGS on the home screen");
	else if (su.pending)
		snprintf(why, n, "setup finishes when the XMB comes up: quit Prism, then open it again");
	else
		return 1;
	return 0;
}

void dev_space_scan(void)
{
	n_dev_space = 3;
	dev_space[0].name = "SD card";
	dev_space[0].free = 6260ull << 20;
	dev_space[1].name = "Internal hard disk";
	dev_space[1].free = 59830ull << 20;
	dev_space[2].name = "USB 0";
	dev_space[2].free = 28120ull << 20;
}

/* ok: everything PS2 needs is here; state: SU_*; pending: our lines not run yet */
void stub_console(int ok, int state, int pending)
{
	memset(&chk, 0, sizeof chk);
	memset(&su, 0, sizeof su);
	chk.done = 1;
	chk.cobra = 1;
	chk.cobra_bcd = 0x0850;
	chk.model = ok ? 2 : 1;
	chk.emu = 1;
	chk.wm = 1;
	chk.flash_stage2 = 1;
	strcpy(chk.fw, "4.93 CEX");
	chk.ps2_ok = ok;
	if (!ok)
		snprintf(chk.why, sizeof chk.why, "this console (%s) runs PS2 games on its own PS2 chips; not supported yet", model_text(chk.model));
	su.state = state;
	su.stage2 = su.carrier = state == SU_NONE ? -1 : 1;
	su.pending = pending;
	su.shipped = 1;
}

/* v0.28 */
int stub_autostart = 1;
int autostart_on(void) { return stub_autostart; }
int autostart_set(int on, char *msg, int n) { (void)on; (void)msg; (void)n; return 0; }

/* 1.00: busy.c (the loading-screen ticker) is console-only: here a loading screen is drawn once per call */
void busy_save(const void *g, const char *head, const char *title, float done, const char *what)
{
	(void)g; (void)head; (void)title; (void)done; (void)what;
}
void busy_lend(void) {}

/* 1.00: Settings "Game names: clean" (setup.c): the PC renderer keeps the setting in memory */
static int stub_names_clean;
int names_clean(void) { return stub_names_clean; }
int names_clean_set(int on) { stub_names_clean = on; return 0; }
int wm_shortcuts_restyle(int was_clean, char *msg, int n) { (void)was_clean; if (n) msg[0] = 0; return 0; }
