/* SETTINGS (v0.15): what the console is, whether PS2 games from the card work here, the one-time
 * setup (set up / remove, each after a question), the storage devices and their free space, a
 * probe log. */
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "look.h"
#include "screens.h"
#include "check.h"
#include "setup.h"
#include "help.h"
#include "version.h"

/* v0.36: "Start Prism with the console" removed (Bananafuzzbuddy, 11-23: no better start-up method exists) */
/* v0.46: Help first (the row the screen opens on) */
enum { ACT_HELP, ACT_SETUP, ACT_RECHECK, ACT_PROBE, ACT_WMFIX, N_ACT };

/* v0.49: the last row is there only while webMAN's Auto-Play is on */
static int n_act(void)
{
	return chk.wm_autoplay == 1 ? N_ACT : N_ACT - 1;
}

static int st_help;	/* the help pages are up (help.c) */

static int st_auto;	/* v0.28: Prism starts with the console (autoexec.bat block) */

static int st_sel, st_ask, st_yes;
static unsigned st_ask_at;

dev_space_t dev_space[DEV_SPACE_N];
int n_dev_space;

void settings_open(void)
{
	st_sel = 0;
	st_ask = 0;
	st_help = 0;
	st_auto = autostart_on();
	dev_space_scan();
}

/* what the PS2 line says, and in which colour */
static const char *ps2_state(col_t *c, char *detail, int n)
{
	detail[0] = 0;
	*c = AMBER;
	if (!chk.ps2_ok)
	{
		snprintf(detail, n, "%s.", chk.why);
		return "Not on this console";
	}
	if (su.state == SU_CONFLICT)
	{
		snprintf(detail, n, "onxmb.bat already loads another PS2 stage2: %s", su.line);
		return "Another tool is in the way";
	}
	if (su.state == SU_NONE)
	{
		snprintf(detail, n, "A few lines in webMAN's onxmb.bat and two small files on the hard disk. Nothing goes to flash.");
		return "Needs a one-time setup";
	}
	if (chk.untested && (su.state == SU_NONE || su.state == SU_APP || su.state == SU_BENCH) && !su.pending && su.stage2 >= 0 && su.carrier >= 0 && su.state != SU_NONE)
	{
		/* v0.49: set up, on a firmware / Cobra it was never run on */
		snprintf(detail, n, "%s", chk.note);
		return "Ready, but not tested here";
	}
	if (su.stage2 < 0 || su.carrier < 0)
	{
		snprintf(detail, n, "A setup file is missing from the hard disk: set up again.");
		return "Setup is not whole";
	}
	if (su.pending)
	{
		snprintf(detail, n, "webMAN runs the new lines when the XMB comes up: quit Prism and open it again.");
		return "Finishes when the XMB comes up";
	}
	*c = GLOW;
	if (su.state == SU_BENCH)
		snprintf(detail, n, "Loaded by onxmb.bat lines Prism did not write (the bench script).");
	else
		snprintf(detail, n, "Pick a PS2 game and press X.");
	return "Ready";
}

/* a long line, cut into at most two that fit w */
static void two_lines(int f, int x, int y, int w, col_t c, const char *s)
{
	char a[200], b[200];
	int n = (int)strlen(s), cut = n, k;

	if (text_w(f, s, 0) <= w)
	{
		text(f, x, y, c, s);
		return;
	}
	for (k = n; k > 0; k--)
	{
		if (s[k] != ' ')
			continue;
		snprintf(a, sizeof a, "%.*s", k, s);
		if (text_w(f, a, 0) <= w)
		{
			cut = k;
			break;
		}
	}
	snprintf(a, sizeof a, "%.*s", cut, s);
	fit(b, sizeof b, f, s + (cut < n ? cut + 1 : n), w);
	text(f, x, y, c, a);
	text(f, x, y + 17, c, b);
}

static void row(int y, const char *k, const char *v, col_t c)
{
	char t[120];

	text(F_SMALL, LEFT, y, DIM, k);
	fit(t, sizeof t, F_SMALL, v, 330 - LEFT - 100);
	text(F_SMALL, LEFT + 100, y, c, t);
}

static void size_txt(char *o, int n, unsigned long long b)
{
	if (b >= (1ull << 30))
		snprintf(o, n, "%.1f GB free", b / 1073741824.0);
	else
		snprintf(o, n, "%llu MB free", b >> 20);
}

void settings_draw(void)
{
	char ln[220], det[220];
	col_t c;
	const char *ps2;
	int i, y, x;

	if (st_help)
	{
		help_draw();
		return;
	}
	top_bar("SETTINGS");
	top_right("PRISM ", DIM, APP_VERSION, FAINT);

	text_ex(F_SMALL, LEFT, 82, 1.0f, 3, FAINT, "THIS CONSOLE");
	row(102, "Model", model_text(chk.model), INK);
	snprintf(ln, sizeof ln, "%s  " DOT "  %s", chk.fw, chk.cobra ? (chk.cobra_bcd == 0x0850 ? "Cobra 8.5" : "Cobra (other)") : "no Cobra");
	row(120, "Firmware", ln, INK);
	/* v0.22: the two webMAN settings Prism's PS3 start depends on (check.c) */
	row(138, "webMAN MOD", !chk.wm ? "not answering" : chk.wm_autoplay == 1 ? "Auto-Play on: turn it off"
		: chk.wm_req == 0 ? "no auto-start: Lite edition?" : "answering",
		chk.wm && chk.wm_autoplay != 1 && chk.wm_req != 0 ? INK : AMBER);
	row(156, "PS2 emulator", chk.emu > 0 ? "matches Prism" : chk.emu == 0 ? "a different one" : "missing", chk.emu > 0 ? INK : AMBER);

	text_ex(F_SMALL, LEFT, 186, 1.0f, 3, FAINT, "PS2 GAMES FROM THE SD CARD");
	ps2 = ps2_state(&c, det, sizeof det);
	text(F_BODY, LEFT, 204, c, ps2);
	two_lines(F_SMALL, LEFT, 228, 290, DIM, det);

	if (st_sel >= n_act())
		st_sel = n_act() - 1;
	for (i = 0; i < n_act(); i++)
	{
		const char *s = i == ACT_HELP ? "Help" : i == ACT_PROBE ? "Write a probe log" : i == ACT_RECHECK ? "Check the console again"
			: i == ACT_WMFIX ? "Turn webMAN's Auto-Play off"
			: (su.state == SU_APP ? "Remove the PS2 setup" : "Set up PS2 play");
		int on = i == st_sel, usable = i != ACT_SETUP || (su.state == SU_APP || (chk.ps2_ok && su.state != SU_CONFLICT && su.shipped));
		const int five = n_act() == 5, pitch = five ? 21 : 25;

		y = (five ? 270 : 274) + i * pitch;
		if (on)
		{
			rect(LEFT, (float)y - 3.0f, 330.0f, (float)y + (five ? 18.0f : 20.0f), RGBA(0x7F, 0xE3, 0xFF, 0x1C));
			frame_rect(LEFT, (float)y - 3.0f, 330.0f, (float)y + (five ? 18.0f : 20.0f), 1.0f, GLOW);
		}
		text(F_BODY, LEFT + 12, y, usable ? (on ? INK : DIM) : FAINT, s);
	}

	x = 360;
	text_ex(F_SMALL, x, 82, 1.0f, 3, FAINT, "STORAGE");
	for (i = 0, y = 102; i < n_dev_space && y < 250; i++, y += 18)
	{
		char t[40];

		text(F_SMALL, x, y, DIM, dev_space[i].name);
		size_txt(t, sizeof t, dev_space[i].free);
		text(F_SMALL, RIGHT - text_w(F_SMALL, t, 0), y, INK, t);
	}
	text_ex(F_SMALL, x, 278, 1.0f, 3, FAINT, "ABOUT");
	text(F_SMALL, x, 298, DIM, "Prism, for PlayStation games on a PS3.");
	text(F_SMALL, x, 316, DIM, "Logs: /dev_hdd0/tmp/prism_*.log");
	text(F_SMALL, x, 334, DIM, "No Sony artwork, logos or sounds.");

	if (note_up())
		text_centre(F_SMALL, 320, 376, 0, AMBER, note_text());
	legend_line();
	x = legend_icon(LEFT, 0, "Choose");
	legend_icon(x, 3, "Menu");

	if (st_ask)
	{
		int setup = su.state != SU_APP;

		rect(LX0, 0.0f, LX1, 448.0f, RGBA(0x02, 0x04, 0x0C, 0x58));
		rect(110.0f, 120.0f, 530.0f, 310.0f, RGBA(0x0D, 0x14, 0x33, 0x80));
		frame_rect(110.0f, 120.0f, 530.0f, 310.0f, 1.0f, LINE);
		spectrum_bar(110.0f, 530.0f, 120.0f, 2.0f, 6, 0x80);
		text_centre(F_BOLD, 320, 136, 0, INK, setup ? "Set up PS2 play from the SD card?" : "Remove the PS2 setup?");
		if (setup)
		{
			text_centre(F_SMALL, 320, 172, 0, DIM, "Prism adds 6 lines at the END of /dev_hdd0/onxmb.bat");
			text_centre(F_SMALL, 320, 190, 0, DIM, "(webMAN runs that file when the XMB comes up), and copies");
			text_centre(F_SMALL, 320, 208, 0, DIM, "its PS2 helper and launcher image to the hard disk (1 MB).");
			text_centre(F_SMALL, 320, 228, 0, GLOW, "Your own lines stay as they are. Nothing goes to flash.");
			if (chk.untested)
				text_centre(F_SMALL, 320, 245, 0, AMBER, "Not tested on this firmware: a PS2 game may not start.");
		}
		else
		{
			text_centre(F_SMALL, 320, 172, 0, DIM, "Prism takes its lines out of /dev_hdd0/onxmb.bat and");
			text_centre(F_SMALL, 320, 190, 0, DIM, "deletes its launcher image. After the next restart PS2");
			text_centre(F_SMALL, 320, 208, 0, DIM, "games run on the normal stage2 again.");
			text_centre(F_SMALL, 320, 230, 0, AMBER, "PS2 games from the SD card stop working.");
		}
		for (i = 0; i < 2; i++)
		{
			float bx = i ? 330.0f : 200.0f;

			if (st_yes == i)
			{
				rect(bx, 262.0f, bx + 110.0f, 292.0f, RGBA(0x7F, 0xE3, 0xFF, 0x1C));
				frame_rect(bx, 262.0f, bx + 110.0f, 292.0f, 1.0f, GLOW);
			}
			text_centre(F_BODY, (int)bx + 55, 268, 0, st_yes == i ? INK : DIM, i ? (setup ? "Set up" : "Remove") : "Not now");
		}
	}
	(void)st_ask_at;
}

void settings_autostart_changed(void)
{
	st_auto = autostart_on();
}

/* DO_* for main, or -1 = back to HOME */
int settings_pad(unsigned pressed)
{
	if (st_ask)
	{
		if (pressed & (PAD_LEFT | PAD_RIGHT))
			st_yes ^= 1;
		if (pressed & PAD_CIRCLE)
			st_ask = 0;
		else if (pressed & PAD_CROSS)
		{
			int setup = su.state != SU_APP;

			st_ask = 0;
			if (st_yes)
				return setup ? DO_SETUP : DO_UNSETUP;
		}
		return DO_NOTHING;
	}
	if (st_help)
	{
		if (help_pad(pressed))
			st_help = 0;
		return DO_NOTHING;
	}
	if (pressed & PAD_CIRCLE)
		return -1;
	if (pressed & PAD_UP)
		st_sel = st_sel ? st_sel - 1 : n_act() - 1;
	if (pressed & PAD_DOWN)
		st_sel = st_sel < n_act() - 1 ? st_sel + 1 : 0;
	if (pressed & PAD_CROSS)
	{
		if (st_sel == ACT_HELP)
		{
			st_help = 1;
			help_open();
			return DO_NOTHING;
		}
		if (st_sel == ACT_WMFIX)
			return DO_WMFIX;
		if (st_sel == ACT_PROBE)
			return DO_PROBE;
		if (st_sel == ACT_RECHECK)
			return DO_RECHECK;
		if (su.state == SU_APP || (chk.ps2_ok && su.state != SU_CONFLICT && su.shipped))
		{
			st_ask = 1;
			st_yes = 0;	/* starts on "Not now" */
			st_ask_at = frame;
		}
		else
			ui_say(!chk.ps2_ok ? "Not on this console (see above)." : su.state == SU_CONFLICT ? "Another tool's lines are in the way (see above)." :
				"This Prism is missing its setup files: reinstall the .pkg.");
	}
	return DO_NOTHING;
}
