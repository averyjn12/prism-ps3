/* Prism app (PS3 side): the launcher's look (screens.c, ported from launcher/ui) over the PS1,
 * PS2 and PS3 games on the SD card in the console's own reader, started straight from here
 * (launch.c). The card is only read. No Sony artwork, logos or sounds. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <io/pad.h>
#include <sysutil/sysutil.h>
#include <sysutil/disc.h>
#include <sys/process.h>
#include "gfx.h"
#include "ui.h"
#include "games.h"
#include "launch.h"
#include "webman.h"
#include "screens.h"
#include "rip.h"
#include "check.h"
#include "setup.h"
#include "ps3sd.h"	/* v0.82 */
#include "manage.h"
#include "version.h"
#include "look.h"
#include "help.h"
#include "intro.h"
#include "intro_sound.h"
#include "covers.h"
#include "pspsd.h"
#include "cardcheck.h"	/* v0.65 */
#include "played.h"	/* v0.67 */
#include "sfx.h"	/* v0.72 */
#include "pad.h"	/* v0.72 */
#include "logcopy.h"	/* v0.81 */
#include "busy.h"	/* 1.00 */

#define TB_HZ		79800000ULL

int run_probe(void);

static volatile int quit;

/* for long jobs outside the main loop (rip.c): let the system's events in; 1 = asked to quit */
int app_events(void)
{
	sysUtilCheckCallback();
	return quit;
}
static spawn_t spawn;

/* v0.40: tell the system this app follows disc changes (cellDiscGameRegisterDiscChangeCallback; in
 * PSL1GHT sysDiscRegisterDiscChangeCallback), as IRISMAN does (source/main.c L2872, its
 * DiscEjectCallback / DiscInsertCallback at L1773-1800). Why: on 11-23 every read of a PS1 or PS2
 * disc put in while Prism ran was refused (602: 0x8001002F, READ CD: -1) with no /dev_bdvd, before
 * and after the drive reset - the system does nothing with a disc put in while an app runs that has
 * not asked for disc changes, and Cobra's own kernel code notes that the lv2 storage functions do
 * not work until the kernel has had the drive's ready event (storage_ext.c L773-775). The calls
 * only log: the drive watch in the main loop does the rest. */
static void on_disc_out(void)
{
	disc_event("system disc event: out", 0, 0);
}

volatile int disc_in_events;	/* v0.42: how many "disc in" events the system has sent (rip_image.h waits on it) */

static void on_disc_in(u32 type, char *title)
{
	disc_in_events++;
	disc_event("system disc event: in; type (0 other, 1 PS3, 2 PS2), title pointer", (unsigned)type, (unsigned)(unsigned long)title);
}

static void on_sysutil(u64 st, u64 param, void *u)
{
	(void)param;
	(void)u;
	if (st == SYSUTIL_EXIT_GAME)
		quit = 1;
}

static u64 tb(void)
{
	u64 t;

	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

/* v0.10: frame times on the console, one line per 5 s to /dev_hdd0/tmp/prism_perf.log (at most
 * 400 lines a run): screen, frames per second, average and worst draw, average copy to the TV.
 * The PC can time ui.c, not the PPU or the copy into RSX memory. */
#define PERF_LOG	"/dev_hdd0/tmp/prism_perf.log"

static void perf(u64 draw, u64 present)
{
	static u64 t_start, d_sum, d_max, p_sum, b_sum, c_sum, f_max;
	static unsigned n, lines, up_sum, drop_sum;
	static int last_screen = -1;
	u64 now = tb();

	if (!t_start)
		t_start = now;
	d_sum += draw;
	b_sum += ui_t_backdrop;
	c_sum += ui_t_bgcopy;
	p_sum += present;
	if (draw > d_max)
		d_max = draw;
	if (draw + present > f_max)
		f_max = draw + present;
	up_sum += gx_stat_uploads;
	drop_sum += gx_stat_dropped;
	n++;
	if ((now - t_start >= 5 * TB_HZ || screen != last_screen) && n > 1 && lines < 400)
	{
		FILE *f = fopen(PERF_LOG, lines ? "a" : "w");

		if (f)
		{
			double secs = (double)(now - t_start) / TB_HZ;

			fprintf(f, "v%s scr %d plat %d: %u frames %.1f fps, draw avg %.1f ms max %.1f ms (backdrop %.1f, of it bg copy %.1f), present avg %.1f ms\n",
				APP_VERSION, last_screen, plat, n, n / secs, d_sum * 1000.0 / TB_HZ / n, d_max * 1000.0 / TB_HZ,
				b_sum * 1000.0 / TB_HZ / n, c_sum * 1000.0 / TB_HZ / n, p_sum * 1000.0 / TB_HZ / n);
			/* v0.67: who drew (gfx.h). With the RSX drawing, "draw" is the PPU building the frame and
			 * "present" is mostly the wait for the vsync; the slowest whole frame says whether one was missed */
			if (gfx_gpu)
				fprintf(f, "   RSX draws: slowest frame %.1f ms, last frame %u vertices in %u draws, %u textures sent, %u vertices dropped\n",
					f_max * 1000.0 / TB_HZ, gx_stat_verts, gx_stat_draws, up_sum, drop_sum);
			else
				fprintf(f, "   CPU draws (reason %d: 1 = asked for with L1 + R1 or soft_draw.txt, negative = RSX set-up step)\n", gfx_gpu_why);
			fclose(f);
			lines++;
		}
		t_start = now;
		d_sum = d_max = p_sum = b_sum = c_sum = f_max = 0;
		n = up_sum = drop_sum = 0;
	}
	last_screen = screen;
}

/* every connected pad, as PAD_* bits. v0.72: pad.c, where a pad with nothing new keeps what it held */
static unsigned buttons(void)
{
	return pad_buttons();
}

static void rescan(const char *what)
{
	progress(NULL, "ONE MOMENT", what, 0.4f, "SD card, hard disk, USB drives and the disc drive");
	games_scan();
	psp_table_write();	/* v0.80: for Prism's XMB plugin (launch.h PSP_TABLE) */
	ui_prepare();
}

static const char *const plat_word[N_PLAT] = { "PS1", "PS2", "PS3", "PSP" };

static int disc_acted, disc_prev;	/* v0.31: the drive watch (main loop) */
static u64 disc_next;

/* after the drive changed: say what is in it now */
static void disc_note(void)
{
	char m[180];
	int p, i;

	for (p = 0; p < N_PLAT; p++)
		for (i = 0; i < n_games[p]; i++)
			if (games[p][i].kind == 4)
			{
				snprintf(m, sizeof m, "In the drive: %s (%s)", games[p][i].shown, plat_word[p]);
				ui_say(m);
				return;
			}
	ui_say("No game disc in the drive");
}

/* returns 1 when the app should end (spawn.self set, or quit) */
static int start(const game_t *g)
{
	char why[200], msg[260];
	int r;

	if (g->kind == 5)
	{
		snprintf(msg, sizeof msg, "Put the %s disc in the drive, then press SELECT to look again.", g->shown);
		ui_say(msg);
		return 0;
	}
	if (slot_bad_msg(slot_of_game(g), "Not started", msg, sizeof msg))
	{
		/* v0.65: the slot gave back damaged data at the last look (cardcheck.h); the game would get the same */
		ui_say(msg);
		return 0;
	}
	if (plat == P_PS3 && WHERE_SLOT(g->where) && g->kind == 3)
	{
		/* v0.22: black screen from the card slot (Borderlands, 2026-11-23); see rip.c ps3_dest. v0.82: an ISO plays from
		 * the card (ps3sd.c); a game FOLDER still cannot */
		ui_say("Not started: a PS3 game folder does not start from a card slot. Put it on the card as an ISO.");
		return 0;
	}
	if (plat == P_PS2 && g->kind < 4 && g->where >= 3 && g->where <= 10)
	{
		/* v0.78: a USB drive holds PS2 games to keep them; neither webMAN nor the card reader plays them there */
		ui_say("Not started: PS2 games do not start from a USB drive. Square: copy it to the hard disk or a card.");
		return 0;
	}
	if (plat == P_PS2 && g->where == 2 && !chk.ps2_ok)
	{
		snprintf(msg, sizeof msg, "Not started: Prism starts PS2 games on CECHC / CECHE consoles only (%s). Start it from webMAN's list on the XMB.", chk.why);
		ui_say(msg);
		return 0;
	}
	if (plat == P_PS2 && g->kind < 4 && g->where != 2 && !ps2_ready(why, sizeof why))
	{
		snprintf(msg, sizeof msg, "Not started: %s.", why);
		ui_say(msg);
		return 0;
	}
	if (plat == P_PSP && (WHERE_SLOT(g->where) || (g->exfat && g->where >= 3 && g->where <= 10))
	    && psp_launcher_for(g) != PSPL_PPSSPP)	/* v0.69: PPSSPP reads no card; launch_prepare says so. v0.82: exFAT USB too */
	{
		/* v0.30: the PSP launcher does not run a game from the card slot (Bananafuzzbuddy, 11-23): play it
		 * from a copy on the hard disk (rip.c psp_card_copy; kept for the next start).
		 * v0.60: first from the card itself, served to webMAN by its sectors (pspsd.c); the copy is
		 * only the fallback when that cannot be done (prism_pspsd.log says why) */
		static game_t copy;
		char pwhy[160];

		copy = *g;
		progress(g, "STARTING", g->shown, 0.2f, "Reading the game on the card");
		if (pspsd_prepare(g, copy.path, sizeof copy.path, pwhy, sizeof pwhy) == 0)
			g = &copy;
		else if (g->exfat || !strstr(pwhy, "plain .iso"))	/* v0.82: an exFAT volume's game has no system file to copy */
		{
			/* v0.64: the card route failed where it should work: say so, copy nothing (Bananafuzzbuddy 11-24: a CF game
			 * started a hard-disk copy instead). Only a .cso, which the card route cannot serve, still copies. */
			snprintf(msg, sizeof msg, "Not started: could not play it from the card: %s. (prism_pspsd.log)", pwhy);
			ui_say(msg);
			return 0;
		}
		else
		{
			copy = *g;
			r = psp_card_copy(buttons, g, copy.path, sizeof copy.path, msg, sizeof msg);
			if (r)
			{
				ui_say(msg);
				return 0;
			}
			copy.where = 2;
			g = &copy;
		}
	}
	progress(g, "STARTING", g->shown, 0.3f, g->kind == 4 ? "The disc in the drive" : "Mounting the game");
	r = launch_prepare(plat, g, &spawn, why, sizeof(why));
	if (r == 0)
		played_mark(plat, g->name, ui_now_s);	/* v0.67: for Continue and "played N times" (played.h) */
	if (r == 0 && spawn.ps2)
	{
		progress(g, "STARTING", g->shown, 0.8f, "Handing over to the PS2 side");
		sound_close();	/* v0.72: the console is about to leave for the PS2 side */
		r = launch_ps2_now();
		if (r == 0)
			for (;;)
				usleep(100000);	/* lv2 is going down into the PS2 LPAR */
		played_undo();	/* it did not start */
		sound_open();
		launch_disarm();
		snprintf(why, sizeof(why), "the PS2 hand-over was refused (lv2 0x%08x)", (u32)r);
	}
	else if (r == 0 && spawn.ppsspp == 2)
	{
		/* v0.80: PPSSPP 314+ starts the game itself (launch.h PPS_BOOT_FILE) */
		progress(g, "STARTING", g->shown, 1.0f, "Starting it in PPSSPP");
		return 1;
	}
	else if (r == 0 && spawn.ppsspp)
	{
		/* v0.69: held long enough to read: the game is picked again in PPSSPP's own list */
		progress(g, "OPENING PPSSPP", g->shown, 1.0f, "PPSSPP shows its own list next: choose this game there");
		usleep(3000000);
		return 1;
	}
	else if (r == 0 && spawn.self[0])
	{
		progress(g, "STARTING", g->shown, 1.0f, plat == P_PS1 ? "Starting the PS1 emulator" : plat == P_PSP ? "Starting the PSP launcher" : "Starting the game");
		return 1;
	}
	else if (r == 0 && spawn.psp)
	{
		/* v0.23: the player starts it; held long enough to read before Prism closes */
		progress(g, "READY", g->shown, 1.0f, "On the XMB, press X on PSP Launcher in the Game column");
		usleep(4000000);
		return 1;
	}
	else if (r == 0)
	{
		progress(g, "STARTING", g->shown, 1.0f, chk.wm_req == 0 ? "On the XMB: press X on the disc icon"
			: "It starts by itself a few seconds after the XMB is back");
		return 1;
	}
	snprintf(msg, sizeof(msg), "%s: not started, %s.", plat_word[plat], why);
	ui_say(msg);
	return 0;
}

int main(int argc, const char *argv[])
{
	unsigned held, last = 0, pressed;
	const unsigned moves = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT;
	int rep = 0, act, snd_open;
	u64 t0, rep_next = 0, wall0 = 0, wall0_tb = 0;

	(void)argc;
	(void)argv;
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, on_sysutil, NULL);
	disc_event("asked the system for disc change events; result", (unsigned)sysDiscRegisterDiscChangeCallback(on_disc_out, on_disc_in), 0);
	t_main_s = wall_s();	/* v0.32: start-up timing (setup.c autostart_started) */
	ui_clock = tb;
	ioPadInit(7);
	snd_open = sound_open();	/* v0.72: for the opening and the menu sounds, until Prism ends */
	{
		/* v0.67: the RSX draws the screens (gfx.h). The way back to the CPU drawing of v0.66, should a
		 * console show nothing or rubbish: hold L1 + R1 while Prism starts (looked at for 0.3 s), or
		 * leave a file USRDIR/soft_draw.txt. Either way the perf log says which path drew. */
		FILE *f = fopen("/dev_hdd0/game/PRSM00001/USRDIR/soft_draw.txt", "rb");
		int k;

		if (f)
		{
			fclose(f);
			gfx_soft_wanted = 1;
		}
		/* v0.69: this version's RSX drawing stopped answering on this console before (gfx.c): the CPU
		 * draws. Another version tries the RSX again. */
		f = fopen(GFX_RSX_FAILED, "rb");
		if (f)
		{
			char v[64] = "";

			if (fgets(v, sizeof v, f) && !strncmp(v, APP_VERSION, strlen(APP_VERSION)))
				gfx_soft_wanted = 1;
			fclose(f);
		}
		for (k = 0; k < 15 && !gfx_soft_wanted; k++)
		{
			unsigned b = buttons();

			if ((b & PAD_L1) && (b & PAD_R1))
				gfx_soft_wanted = 1;
			usleep(20000);
		}
	}
	if (gfx_init() != 0)
		return 1;
	{
		/* 1.00: the loading screens keep moving while Prism waits (busy.c) */
		int bz = busy_init();
		FILE *f = fopen("/dev_hdd0/tmp/prism_gfx.log", "a");

		if (f)
		{
			fprintf(f, "busy ticker: %d (0 = on; -1 = the CPU draws)\n", bz);
			fclose(f);
		}
	}
	launch_disarm();
	launch_cancel();
	{
		/* v0.28: the opening (intro.c), on real time; a new button press ends it.
		 * v0.29: INTRO_LEAD seconds of black first (Bananafuzzbuddy, 11-23: it "started too early", the TV is
		 * still dark while the console swaps the XMB for the app); presses in the first half second
		 * do not count; how it ran goes to /dev_hdd0/tmp/prism_intro.log. */
		/* v0.30: its sound (intro_synth.c), computed before the clock starts and started at t = 0
		 * (a little early: the port plays ~85 ms behind what is written) */
		u64 synth0 = tb();
		int snd_prep = intro_sound_prepare(), snd_play = -9, snd_on = 0;
		double synth_ms = (double)(tb() - synth0) * 1000.0 / TB_HZ;
		u64 s = tb(), prev = s, worst = 0;
		unsigned before = buttons(), frames = 0;
		float t = 0.0f, tr = 0.0f;
		int by_button = 0;

		while (!quit)
		{
			u64 now_tb = tb();
			unsigned now;

			if (now_tb - prev > worst)
				worst = now_tb - prev;
			prev = now_tb;
			tr = (float)(now_tb - s) / (float)TB_HZ;
			t = tr - INTRO_LEAD;
			sysUtilCheckCallback();
			if (!snd_on && snd_prep >= 0 && t >= -0.02f)	/* v0.72: the port is 20 ms behind now, not 85 */
			{
				snd_play = intro_sound_play();
				snd_on = 1;
				t_intro_s = wall_s();	/* v0.32: for the start-up timing line */
			}
			now = buttons();
			if (t >= INTRO_LEN || gfx_dead)
				break;
			if ((now & ~before) && tr > 0.5f)
			{
				by_button = 1;
				break;
			}
			before = now;
			intro_draw(t);
			gfx_present();
			frames++;
		}
		intro_sound_end(by_button);
		{
			FILE *f = fopen("/dev_hdd0/tmp/prism_intro.log", "w");

			if (f)
			{
				fprintf(f, "v%s intro: %u frames in %.2f s (%.1f fps; lead-in %.1f s, intro %.1f s), slowest frame %.0f ms, ended by %s at intro t = %.2f s\n",
					APP_VERSION, frames, tr, tr > 0.0f ? frames / tr : 0.0f, (double)INTRO_LEAD, (double)INTRO_LEN,
					worst * 1000.0 / TB_HZ, by_button ? "a button" : "time", t);
				fprintf(f, "sound: %s (%d) in %.0f ms, port %d, started %d\n", snd_prep == 1 ? "loaded" : snd_prep == 0 ? "computed" : "none",
					snd_prep, synth_ms, snd_open, snd_play);
				fclose(f);
			}
		}
	}
	if (!quit && !terms_accepted())
	{
		/* v0.46: the terms of use, once per console, before Prism looks at or touches anything
		 * (help.c). X counts only after three seconds; Circle leaves Prism. */
		unsigned before = buttons();
		u64 s = tb();
		int agreed = 0;

		while (!quit && !agreed)
		{
			unsigned now = buttons(), hit = now & ~before;
			int armed = tb() - s > 3 * (u64)TB_HZ;

			before = now;
			sysUtilCheckCallback();
			frame++;
			backdrop();
			terms_draw(armed);
			gfx_present();
			if (hit & PAD_CIRCLE)
				break;
			if (armed && (hit & PAD_CROSS))
				agreed = 1;
		}
		if (!agreed)
		{
			gfx_end();
			ioPadEnd();
			return 0;
		}
		terms_accept();
	}
	t0 = tb();
	progress(NULL, "ONE MOMENT", "Checking the console", 0.2f, "Model, firmware, Cobra, webMAN, the PS2 emulator");
	check_console();
	autostart_started();	/* v0.29: before setup_read, it may rewrite onxmb.bat */
	setup_read();
	{
		/* v0.82: an update's new PS2 helper replaces the installed one (setup.c setup_refresh) */
		char rm[200];

		if (setup_refresh(rm, sizeof rm) != 0)
			ui_say(rm);
	}
	ps3sd_release();	/* v0.82: before games_scan reads the cards (ps3sd.c) */
	tinytees_install();
	art_load();
	tinytees_cover();	/* v0.59: loads the art file again itself if it added the cover */
	pps_links_clean();	/* v0.72: PPSSPP's empty stand-in files, once the console has restarted (launch.h) */
	played_load();		/* v0.67: before the first ui_prepare(), which looks for the game to continue */
	ui_names_clean = names_clean();	/* 1.00: Settings "Game names: clean" */
	wall0 = wall_s();
	wall0_tb = tb();
	ui_now_s = wall0;
	rescan("Looking for games");
	disc_acted = disc_prev = disc_signature();
	disc_next = tb() + TB_HZ * 3 / 2;

	while (!quit)
	{
		/* v0.31: a disc that goes in or comes out while Prism is open (Bananafuzzbuddy, 11-23: a PS1 and a
		 * PS2 disc put in were not listed; the drive was looked at only at the start and on
		 * SELECT). Every 1.5 s; acted on once the change has held for two looks, and only on the
		 * home and games screens. */
		if (tb() >= disc_next)
		{
			int sig = disc_signature();

			disc_next = tb() + TB_HZ * 3 / 2;
			if (sig != disc_acted && sig == disc_prev && (screen == SCR_HOME || screen == SCR_GAMES))
			{
				disc_acted = sig;
				rescan("The disc drive changed");
				disc_note();
				last = buttons();
			}
			disc_prev = sig;
		}
		if (gfx_dead)
			break;	/* v0.68: the RSX stopped answering: close, do not hang (gfx.c, prism_gfx.log) */
		frame = (unsigned)((tb() - t0) * 60 / TB_HZ);
		ui_now_s = wall0 + (tb() - wall0_tb) / TB_HZ;	/* v0.67: wall_s() itself writes a file: once, at the start */
		sysUtilCheckCallback();
		held = buttons();
		pressed = held & ~last;
		if (held & moves)
		{
			/* v0.67: by the clock. It was counted in frames (13 frames, then every other one), which at
			 * the 20 frames a second of v0.66 was 0.65 s and then 10 a second; at 60 it would be 0.2 s
			 * and 30 a second, too quick to stop on a game. Now 0.4 s, then 11 a second, at any rate. */
			u64 now = tb();

			if (!rep)
			{
				rep = 1;
				rep_next = now + TB_HZ * 4 / 10;
			}
			else if (now >= rep_next)
			{
				pressed |= held & moves;
				rep_next = now + TB_HZ * 9 / 100;
			}
		}
		else
			rep = 0;
		last = held;

		{
			/* v0.72: a sound whenever a press moved or changed something on the screen (sfx.h): a tick for
			 * a move, two notes up for a choice, two down for going back */
			unsigned before = ui_nav_sig();

			act = ui_pad(pressed, held);
			if (pressed && (act != DO_NOTHING || ui_nav_sig() != before))
				sfx((pressed & PAD_CIRCLE) ? SFX_BACK : (pressed & (moves | PAD_L1 | PAD_R1)) ? SFX_MOVE : SFX_OK);
			intro_sound_tidy();
		}
		if (act == DO_QUIT)
			break;
		if (act == DO_RESCAN)
		{
			char m[80];

			rescan("Looking at the card again");
			snprintf(m, sizeof m, "Found %d PS1, %d PS2, %d PSP and %d PS3 game(s)", n_games[P_PS1], n_games[P_PS2], n_games[P_PSP], n_games[P_PS3]);
			ui_say(m);
			last = buttons();
		}
		else if (act == DO_RIP)
		{
			char m[200];

			rip_disc(buttons, m, sizeof m);
			rescan("Looking at the card again");
			ui_say(m);
			last = buttons();
		}
		else if (act == DO_SETUP || act == DO_UNSETUP)
		{
			char m[220];

			progress(NULL, "ONE MOMENT", act == DO_SETUP ? "Setting up PS2 play" : "Removing the PS2 setup", 0.5f, "/dev_hdd0/onxmb.bat");
			if (act == DO_SETUP)
				setup_install(m, sizeof m);
			else
				setup_remove(m, sizeof m);
			ui_say(m);
			last = buttons();
		}
		else if (act == DO_AUTOSTART)
		{
			char m[200];

			/* v0.28: flip "Start Prism with the console" */
			autostart_set(!autostart_on(), m, sizeof m);
			setup_read();	/* v0.29: the switch writes onxmb.bat too */
			settings_autostart_changed();
			ui_say(m);
			last = buttons();
		}
		else if (act == DO_WMFIX)
		{
			char m[200];

			progress(NULL, "ONE MOMENT", "Turning webMAN's Auto-Play off", 0.5f, "webMAN MOD");
			wm_autoplay_off(m, sizeof m);
			settings_open();
			ui_say(m);
			last = buttons();
		}
		else if (act == DO_WMSC)
		{
			/* v0.55: the card's PS2 games in webMAN's list (setup.c wm_shortcuts_add) */
			char m[220];

			progress(NULL, "ONE MOMENT", "Putting the SD card's PS2 games in webMAN's list", 0.5f, "/dev_hdd0/PS2ISO/SDCARD");
			wm_shortcuts_add(m, sizeof m);
			ui_say(m);
			last = buttons();
		}
		else if (act == DO_RECHECK)
		{
			progress(NULL, "ONE MOMENT", "Checking the console", 0.5f, "Model, firmware, Cobra, webMAN, the PS2 emulator");
			check_console();
			setup_read();
			settings_open();
			ui_say(chk.ps2_ok ? "Checked: PS2 games from the card can work here." : "Checked.");
			last = buttons();
		}
		else if (act == DO_COV_FIND || act == DO_COV_GET)
		{
			/* v0.57: SETTINGS, Game covers (covers.c) */
			char m[240];

			if (act == DO_COV_FIND)
				covers_find(buttons, m, sizeof m);
			else if (covers_get(buttons, m, sizeof m))
			{
				rescan("Putting the new covers on the games");	/* art_load moved every cover pointer */
				covers_open();
			}
			ui_say(m);
			last = buttons();
		}
		else if (act == DO_PSPL_REM || act == DO_PSPL_MIN || act == DO_PSPL_PPS)
		{
			/* v0.61: this game's own PSP launcher; the same as the default = no line of its own */
			const game_t *g = (const game_t *)ui_chosen();
			char m[220];

			if (g)
			{
				int want = act == DO_PSPL_REM ? PSPL_REMASTER : act == DO_PSPL_PPS ? PSPL_PPSSPP : PSPL_MINIS_, def = psp_launcher_for(NULL);

				if (psp_launcher_set(psp_game_key(g), want == def ? 0 : want) != 0)
					snprintf(m, sizeof m, "Could not save the choice.");
				else if (want == PSPL_PPSSPP && ppsspp_direct())	/* v0.80 */
					snprintf(m, sizeof m, "%s will start in PPSSPP.", g->shown);
				else if (want == PSPL_PPSSPP)	/* v0.69 */
					snprintf(m, sizeof m, "%s will open in PPSSPP: choose it again in PPSSPP's own list.", g->shown);
				else
					snprintf(m, sizeof m, "%s will start with the %s launcher.", g->shown, want == PSPL_REMASTER ? "Remaster" : "Minis");
				ui_say(m);
			}
			last = buttons();
		}
		else if (act == DO_COV_DEL || act == DO_COV_ALLOW)
		{
			/* v0.58: remove a wrong cover for good, or let it be looked up again (covers.c covers_mark) */
			const game_t *g = (const game_t *)ui_chosen();
			static game_t keep;
			char m[220];

			if (g)
			{
				keep = *g;
				if (covers_mark(&keep, plat, act == DO_COV_DEL, m, sizeof m))
					rescan("Putting the covers on the games");	/* art_load moved every cover pointer */
				ui_say(m);
			}
			last = buttons();
		}
		else if (act == DO_WM_ADD || act == DO_WM_DEL)
		{
			/* v0.56: one card PS2 game in / out of webMAN's list (setup.c wm_shortcut_one) */
			const game_t *g = (const game_t *)ui_chosen();
			char m[220];

			if (g)
			{
				progress(NULL, "ONE MOMENT", g->shown, 0.5f, "webMAN's PS2 list, folder SDCARD");
				wm_shortcut_one(g, act == DO_WM_ADD, m, sizeof m);
				ui_say(m);
			}
			last = buttons();
		}
		else if (act == DO_WM_ADD_MARKED || act == DO_WM_DEL_MARKED)
		{
			/* v0.78: every marked card PS2 game (setup.c wm_shortcut_many) */
			static game_t batch[MAX_GAMES];
			char m[320];
			int n = ui_marked(batch, MAX_GAMES);

			progress(NULL, "ONE MOMENT", "The marked games", 0.5f, "webMAN's PS2 list, folder SDCARD");
			wm_shortcut_many(batch, n, act == DO_WM_ADD_MARKED, m, sizeof m);
			ui_say(m);
			last = buttons();
		}
		else if (act == DO_DELETE_ASK || act == DO_DELETE || act == DO_COPYGAME || act == DO_SC_ADD || act == DO_SC_DEL)
		{
			const game_t *g = (const game_t *)ui_chosen();
			char m[220];
			static game_t keep;

			if (g)
			{
				keep = *g;	/* the list is rebuilt below */
				m[0] = 0;
				if (act == DO_DELETE_ASK)
				{
					progress(NULL, "ONE MOMENT", keep.shown, 0.5f, "Finding its files");
					if (game_del_plan(plat, &keep, m, sizeof m) == 0)
						ui_ask_delete(m);
					else
						ui_say(m);
				}
				else
				{
					if (act == DO_DELETE)
						game_delete(buttons, plat, &keep, m, sizeof m);
					else if (act == DO_COPYGAME)
						copy_game(buttons, plat, &keep, m, sizeof m);
					else if (act == DO_SC_ADD)
						shortcut_add(plat, &keep, m, sizeof m);
					else
						shortcut_del(plat, &keep, m, sizeof m);
					rescan("Looking for games again");
					ui_say(m);
				}
			}
			last = buttons();
		}
		else if (act == DO_DELMARKED_ASK || act == DO_DELMARKED)
		{
			/* v0.75: every game marked with R2. First each one is planned as a single delete is (its files
			 * found; a mounted game, a disc or a bad card slot refuses): one that cannot go stops the whole
			 * thing before any question. Then the same two questions, once, for all; then one by one.
			 * Circle during the deleting stops it where it is. */
			static game_t batch[MAX_GAMES];
			char m[360], w[220];
			int n = ui_marked(batch, MAX_GAMES), i, done = 0;

			m[0] = 0;
			if (act == DO_DELMARKED_ASK)
			{
				unsigned long long bytes = 0;

				for (i = 0; i < n && !m[0]; i++)
				{
					progress(NULL, "ONE MOMENT", batch[i].shown, n ? (float)i / n : 0.5f, "Finding its files");
					if (game_del_plan(plat, &batch[i], w, sizeof w))
						snprintf(m, sizeof m, "%s cannot be deleted (%s). Take its mark off with R2, or nothing is deleted.", batch[i].shown, w);
					else
						bytes += game_del_bytes();
				}
				if (m[0] || !n)
					ui_say(m[0] ? m : "No games are marked.");
				else
				{
					if (bytes >= 1ull << 30)
						snprintf(w, sizeof w, "%.1f GB in all, from this list", bytes / 1073741824.0);
					else
						snprintf(w, sizeof w, "%.0f MB in all, from this list", bytes / 1048576.0);
					ui_ask_delete_many(n, w);
				}
			}
			else
			{
				for (i = 0; i < n; i++)
				{
					if (game_delete(buttons, plat, &batch[i], w, sizeof w))
						break;
					done++;
				}
				rescan("Looking for games again");
				if (done == n)
					snprintf(m, sizeof m, "Deleted %d game%s.", n, n == 1 ? "" : "s");
				else
					snprintf(m, sizeof m, "Deleted %d of %d. It stopped at %s: %s", done, n, batch[done].shown, w);
				ui_say(m);
			}
			last = buttons();
		}
		else if (act == DO_COPYMARKED)
		{
			/* v0.67: the games marked with R2, to one device, one after another (rip.c copy_games) */
			static game_t batch[MAX_GAMES];
			char m[320];
			int n = ui_marked(batch, MAX_GAMES);

			copy_games(buttons, plat, batch, n, m, sizeof m);
			rescan("Looking for games again");
			ui_say(m);
			last = buttons();
		}
		else if (act == DO_PROBE)
		{
			char m[80];

			/* v0.81: a fresh console check and the probe, then every Prism log to a USB drive (logcopy.c) */
			progress(NULL, "ONE MOMENT", "Writing the logs", 0.3f, "Model, firmware, Cobra, webMAN, the PS2 emulator");
			check_console();
			setup_read();
			progress(NULL, "ONE MOMENT", "Writing the logs", 0.5f, "/dev_hdd0/tmp/prism_app.log");
			run_probe();
			progress(NULL, "ONE MOMENT", "Copying the logs", 0.8f, "To PRISM_LOGS on a USB drive");
			logs_to_usb(m, sizeof m);
			settings_open();
			ui_say(m);
			last = buttons();
		}
		else if (act == DO_START)
		{
			const game_t *g = (const game_t *)ui_chosen();

			if (g && start(g))
				break;
			last = buttons();
		}
		mc_step();
		{
			u64 a = tb(), b, c;

			ui_draw();
			b = tb();
			gfx_present();
			c = tb();
			perf(b - a, c - b);
		}
	}

	sound_close();
	gfx_end();
	ioPadEnd();
	wm_end();
	if (spawn.self[0])
		sysProcessExitSpawn2(spawn.self, spawn.argv, NULL, NULL, 0, 1001, SYS_PROCESS_SPAWN_STACK_SIZE_1M);
	(void)APP_VERSION;
	return 0;
}
