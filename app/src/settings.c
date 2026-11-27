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
#include "covers.h"
#include "launch.h"
#include "sfx.h"	/* v0.73: menu_sounds_on */
#include "version.h"

/* v0.36: "Start Prism with the console" removed (Bananafuzzbuddy, 11-23: no better start-up method exists) */
/* v0.46: Help first (the row the screen opens on) */
/* v0.55: the card's PS2 games in webMAN's list, right under the setup it needs; v0.57: Game covers */
/* v0.61: the PSP launcher games start with (launch.c psp_launcher_*) */
/* v0.80: ACT_PPSD, whether PPSSPP starts the game at once (launch.h PPS_BOOT_FILE) or opens its own list */
enum { ACT_HELP, ACT_SETUP, ACT_WMSC, ACT_COVERS, ACT_PSPL, ACT_PPSD, ACT_SOUND, ACT_RECHECK, ACT_PROBE, ACT_WMFIX, N_ACT };	/* v0.73: ACT_SOUND */

static int st_covers;	/* v0.57: the Game covers page is up (covers.c) */

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

/* v0.70: which PSP launchers are installed and which one is the default: both mean opening or looking up
 * files (launch.c), and v0.61-v0.69 asked once for EVERY ROW of EVERY FRAME. On the console the Settings
 * screen took 12.5 ms of a frame against 1.5 ms for HOME (prism_perf.log, 2026-11-25, scr 4), nearly all
 * of a 60 Hz frame. Now asked when the screen opens, after the row is used, and once a second. */
static int pl_have, pl_now;
static int pp_build, pp_on;	/* v0.80: the installed PPSSPP's build, and the start-it-outright setting */
static unsigned pl_at;

static void pl_refresh(void)
{
	pl_have = psp_launchers_installed();
	pl_now = psp_launcher_for(NULL);
	pp_build = (pl_have & PSPL_PPSSPP) ? ppsspp_build() : 0;
	pp_on = ppsspp_direct_on();
	pl_at = frame;
}

unsigned settings_nav_sig(void)	/* v0.72: screens.h */
{
	return (unsigned)(st_sel * 64 + st_ask * 8 + st_yes * 4 + st_help * 2 + st_covers) + help_nav_sig() * 1024u + (unsigned)pl_now * 65536u
		+ (unsigned)menu_sounds_on() * 524288u + (unsigned)pp_on * 1048576u;
}

void settings_open(void)
{
	pl_refresh();
	st_sel = 0;
	st_ask = 0;
	st_help = 0;
	st_covers = 0;
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
	if (st_covers)
	{
		covers_draw();
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

	if (frame - pl_at > 60)
		pl_refresh();
	if (st_sel >= n_act())
		st_sel = n_act() - 1;
	for (i = 0; i < n_act(); i++)
	{
		const int pl = pl_now;
		const char *s = i == ACT_HELP ? "Help" : i == ACT_PROBE ? "Write logs, copy to USB" : i == ACT_RECHECK ? "Check the console again"
			: i == ACT_WMFIX ? "Turn webMAN's Auto-Play off" : i == ACT_WMSC ? "Put PS2 games in webMAN's list"
			: i == ACT_COVERS ? "Game covers"
			: i == ACT_SOUND ? (menu_sounds_on() ? "Menu sounds: on" : "Menu sounds: off")
			: i == ACT_PPSD ? (!(pl_have & PSPL_PPSSPP) ? "PPSSPP: not installed" : !pp_on ? "PPSSPP: opens its game list first"
				: pp_build >= PPS_DIRECT_BUILD ? "PPSSPP: starts the game outright" : "PPSSPP: start outright (needs build 314)")
			: i == ACT_PSPL ? (pl == PSPL_REMASTER ? "PSP games start with: Remaster launcher" : pl == PSPL_PPSSPP ? "PSP games start with: PPSSPP"
				: pl ? "PSP games start with: Minis launcher" : "PSP games: no PSP launcher installed")
			: (su.state == SU_APP ? "Remove the PS2 setup" : "Set up PS2 play");
		int on = i == st_sel, usable = (i != ACT_SETUP || (su.state == SU_APP || (chk.ps2_ok && su.state != SU_CONFLICT && su.shipped)))
			&& (i != ACT_WMSC || su.state == SU_APP || su.state == SU_BENCH)
			&& (i != ACT_PSPL || (pl_have & (pl_have - 1)) != 0)	/* v0.69: two or more to choose from */
			&& (i != ACT_PPSD || (pl_have & PSPL_PPSSPP));		/* v0.80 */
		/* v0.62 (Bananafuzzbuddy 11-24: the bar no longer lined up): every row is a slot of the same height, sized from
		 * the font's own height and the room between the PS2 lines and the note line; the bar IS the slot and
		 * the text sits in its middle, so the two cannot drift apart however many rows there are. Seven rows
		 * or more use the small font, and a row too wide for the column is cut to fit */
		/* the list starts under the PS2 lines: one line of detail (228) or two (245) */
		const int rows = n_act(), f = rows >= 7 ? F_SMALL : F_BODY, th = font_h(f);
		const int top = text_w(F_SMALL, det, 0) <= 290 ? 252 : 266, room = 374 - top;
		int pitch = room / rows, ty;
		char cut[120];

		if (pitch > th + 12)
			pitch = th + 12;
		y = top + i * pitch;
		ty = y + (pitch - th) / 2;	/* the text's top, in the middle of its slot */
		if (on)
		{
			/* the bar wraps the text: 3 above, 2 below, never past the slot */
			float b0 = (float)(ty - 3 > y ? ty - 3 : y), b1 = (float)(ty + th + 2 < y + pitch ? ty + th + 2 : y + pitch);

			rect(LEFT, b0, 330.0f, b1, RGBA(0x7F, 0xE3, 0xFF, 0x1C));
			frame_rect(LEFT, b0, 330.0f, b1, 1.0f, GLOW);
		}
		fit(cut, sizeof cut, f, s, 330 - LEFT - 20);
		text(f, LEFT + 12, ty, usable ? (on ? INK : DIM) : FAINT, cut);
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
	if (st_covers)
	{
		int r = covers_pad(pressed);

		if (r < 0)
		{
			st_covers = 0;
			return DO_NOTHING;
		}
		return r;
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
		if (st_sel == ACT_PSPL)
		{
			/* v0.61: flips the default between the two launchers; a game's own choice (Square) stays.
			 * v0.69: goes round every installed one: Minis, Remaster, PPSSPP */
			static const int order[3] = { PSPL_MINIS_, PSPL_REMASTER, PSPL_PPSSPP };
			int have = psp_launchers_installed(), now = psp_launcher_for(NULL), next = now, k, at = 0;

			for (k = 0; k < 3; k++)
				if (order[k] == now)
					at = k;
			for (k = 1; k <= 3; k++)
				if (have & order[(at + k) % 3])
				{
					next = order[(at + k) % 3];
					break;
				}
			if (!(have & (have - 1)))
				ui_say(!have ? "No PSP launcher is installed: webMAN's setup can install them."
					: have == PSPL_MINIS_ ? "Only the Minis launcher (PSPM66820) is installed."
					: have == PSPL_REMASTER ? "Only the Remaster launcher (PSPC66820) is installed." : "Only PPSSPP is installed.");
			else if (psp_launcher_set(NULL, next) == 0)
				ui_say(next == PSPL_REMASTER ? "PSP games start with the Remaster launcher (PSPC66820), unless a game has its own choice."
					: next == PSPL_PPSSPP ? (ppsspp_direct() ? "PSP games start in PPSSPP, unless a game has its own choice."
						: "PSP games open in PPSSPP (choose the game again in its list), unless a game has its own choice.")
					: "PSP games start with the Minis launcher (PSPM66820), unless a game has its own choice.");
			else
				ui_say("Could not save the setting.");
			pl_refresh();
			return DO_NOTHING;
		}
		if (st_sel == ACT_PPSD)
		{
			/* v0.80: PPSSPP starts the game outright (launch.h PPS_BOOT_FILE), or opens its own list */
			int on = !ppsspp_direct_on();

			pl_refresh();
			if (!(pl_have & PSPL_PPSSPP))
				ui_say("PPSSPP is not installed.");
			else if (ppsspp_direct_set(on) != 0)
				ui_say("Could not save the setting.");
			else if (!on)
				ui_say("PSP games opened in PPSSPP show its own game list first: choose the game again there.");
			else if (pp_build >= PPS_DIRECT_BUILD)
				ui_say("PSP games opened in PPSSPP start at once, without its game list.");
			else
				ui_say("Saved, but this PPSSPP cannot be handed a game: install build 314 or later. Until then it shows its list.");
			pl_refresh();
			return DO_NOTHING;
		}
		if (st_sel == ACT_SOUND)
		{
			/* v0.73: the tick, the two notes up and the two notes down; the opening keeps its sound */
			int on = !menu_sounds_on();

			if (menu_sounds_set(on) == 0)
				ui_say(on ? "Menu sounds are on." : "Menu sounds are off. The opening keeps its sound.");
			else
				ui_say("Could not save the setting: it holds until Prism closes.");
			return DO_NOTHING;
		}
		if (st_sel == ACT_COVERS)
		{
			st_covers = 1;
			covers_open();
			return DO_NOTHING;
		}
		if (st_sel == ACT_WMSC)
		{
			if (su.state == SU_APP || su.state == SU_BENCH)
				return DO_WMSC;
			ui_say("Set up PS2 play first (the row above).");
			return DO_NOTHING;
		}
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
