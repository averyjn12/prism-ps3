/* Render the app's screens on the PC (same ui.c / screens.c / mcard.c / art.c as the console
 * build) into PPM files, to look at before anything goes on the console.
 *   render <art file> <out dir>      writes home.ppm, games_ps2.ppm, ps2_next.ppm, search.ppm,
 *                                      empty_ps3.ppm, starting.ppm, mc.ppm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gfx.h"
#include "games.h"
#include "cardcheck.h"	/* v0.65: LR_E_BADSLOT, card_bad_slot */
#include "screens.h"
#include "vmc.h"
#include "setup.h"
#include "check.h"
#include "intro.h"
#include "look.h"
#include "help.h"
void stub_console(int ok, int state, int pending);

static const char *card_files[8];
static int n_card_files;

int vmc_load_all(void)
{
	int i;

	for (i = 0; i < n_cards; i++)
		vmc_free(&cards[i]);
	n_cards = 0;
	for (i = 0; i < n_card_files; i++)
		if (vmc_load(&cards[n_cards], card_files[i]) == 0 || cards[n_cards].raw)
			n_cards++;
	return n_cards;
}

u32 *fb;
game_t games[N_PLAT][MAX_GAMES];
int n_games[N_PLAT];
int card_state;
static const char *outdir;
static const char *next_name;

int gfx_init(void) { fb = calloc(CW * CH, 4); return 0; }
void gfx_end(void) {}

static void save(const char *name)
{
	char p[512];
	FILE *f;
	int i;

	snprintf(p, sizeof p, "%s/%s.ppm", outdir, name);
	f = fopen(p, "wb");
	fprintf(f, "P6\n%d %d\n255\n", CW, CH);
	for (i = 0; i < CW * CH; i++)
	{
		unsigned char px[3] = { (unsigned char)(fb[i] >> 16), (unsigned char)(fb[i] >> 8), (unsigned char)fb[i] };

		fwrite(px, 1, 3, f);
	}
	fclose(f);
}

void gfx_present(void)
{
	if (next_name)
	{
		save(next_name);
		next_name = NULL;
	}
}

void games_scan(void) {}
const char *where_name(int w) { static const char *const n[] = { "SD", "SD", "HDD", "USB 0", "USB 1", "USB 2", "USB 3", "USB 4", "USB 5", "USB 6", "USB 7", "DISC", "MS", "CF" }; return w >= 0 && w < 14 ? n[w] : "?"; }

static game_t *add(int p, const char *name, const char *id, unsigned long long size, int kind)
{
	game_t *g = &games[p][n_games[p]++];

	memset(g, 0, sizeof(*g));
	snprintf(g->name, sizeof g->name, "%s", name);
	snprintf(g->id, sizeof g->id, "%s", id);
	g->size = size;
	g->kind = (unsigned char)kind;
	return g;
}

static void shot(const char *name, unsigned at_frame)
{
	frame = at_frame;
	ui_draw();
	save(name);
}

/* v0.66: screenshots to show Prism to people (render.sh <the console's own art file> <dir> showcase): a real library
 * with real sizes, the console in its ordinary state (webMAN's request file served, Auto-Play off) */
static int showcase(void)
{
	game_t *g;
	int k;

	g = add(P_PS2, "GTASA.ISO", "SLUS_209.46", 4501209088ULL, 0);
	g->where = 1, g->slot = 1;
	g = add(P_PS2, "Guitar Hero II (SLUS_214.47).iso", "SLUS_214.47", 4646305792ULL, 0);
	g->where = 1, g->slot = 1;
	g = add(P_PS2, "Metal Gear Solid 2 - Substance (USA).iso", "SLUS_205.54", 6436749312ULL, 0);
	g->where = 1, g->slot = 1;
	g = add(P_PS2, "Metal Gear Solid 3 Subsistence (Disc 2) (Persistence) (SLUS_212.43).iso", "SLUS_212.43", 2999189504ULL, 0);
	g->where = 1, g->slot = 1;
	add(P_PS2, "Tiny Tees.iso", "SLUS_999.01", 1736704ULL, 0)->where = 2;
	add(P_PS1, "SLUS_000.76.bin", "SLUS_000.76", 612552528ULL, 1)->where = 0;
	add(P_PS1, "Metal Gear Solid (USA) (Disc 1) (Rev 1).bin", "SLUS_005.94", 705614112ULL, 1)->where = 0;
	add(P_PSP, "Spider-Man 3 (USA).iso", "ULUS10317", 1683259392ULL, 0)->where = 0;
	add(P_PSP, "Call of Duty - Roads to Victory (USA).iso", "ULUS10218", 640974848ULL, 0)->where = 0;
	add(P_PS3, "Borderlands\xc2\xae", "BLUS30386", 0, 3)->where = 2;
	card_state = 0;
	card_bad_slot = -1;
	card_read_mask = 2;
	stub_console(1, SU_APP, 0);
	chk.wm_req = 1;
	chk.wm_autoplay = 0;
	ui_prepare();

	intro_draw(2.9f);
	save("01_intro");
	ui_home_go(1);
	shot("02_home", 120);
	ui_pad(PAD_CROSS, 0);			/* PS2 */
	shot("03_ps2_games", 300);
	ui_pad(PAD_RIGHT, 0);
	shot("04_ps2_next", 420);
	ui_pad(PAD_SQUARE, 0);			/* its options */
	shot("05_ps2_options", 440);
	ui_pad(PAD_CIRCLE, 0);
	ui_pad(PAD_CIRCLE, 0);
	ui_home_go(0);				/* PS1 */
	ui_pad(PAD_CROSS, 0);
	shot("06_ps1_games", 600);
	ui_pad(PAD_CIRCLE, 0);
	ui_home_go(2);				/* PSP */
	ui_pad(PAD_CROSS, 0);
	shot("07_psp_games", 700);
	ui_pad(PAD_CIRCLE, 0);
	ui_home_go(3);				/* PS3 */
	ui_pad(PAD_CROSS, 0);
	shot("08_ps3_games", 800);
	ui_pad(PAD_CIRCLE, 0);
	ui_home_go(5);				/* SETTINGS */
	ui_pad(PAD_CROSS, 0);
	shot("09_settings", 900);
	for (k = 0; k < 3; k++)
		ui_pad(PAD_DOWN, 0);
	ui_pad(PAD_CROSS, 0);			/* Game covers */
	shot("10_game_covers", 1000);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 3)
		return 2;
	outdir = argv[2];
	gfx_init();
	printf("art records: %d\n", art_load_from(argv[1]));
	if (argc > 3 && !strcmp(argv[3], "showcase"))
		return showcase();
	add(P_PS2, "Metal Gear Solid 2 - Substance (USA).iso", "SLUS_205.54", 6436749312ULL, 0);
	add(P_PS2, "GTASA.ISO", "SLUS_209.46", 4501209088ULL, 0);
	add(P_PS2, "Crazy Taxi (USA).bin", "", 700000000ULL, 2);
	add(P_PS2, "Dynasty Warriors 4 (Disc 1).iso", "SLUS_206.53", 4000000000ULL, 0);
	add(P_PS2, "Tiny Tees.iso", "SLUS_999.01", 1376256ULL, 0)->where = 2;	/* v0.16: on the hard disk */
	add(P_PS1, "Metal Gear Solid (USA) (Disc 1) (Rev 1).bin", "SLUS_005.94", 705614112ULL, 1);
	add(P_PS3, "Borderlands\xc2\xae", "BLUS30386", 0, 3)->where = 2;	/* a /dev_hdd0/GAMES folder, TITLE as in its PARAM.SFO */
	{
		game_t *d = add(P_PS3, "Borderlands\xe2\x84\xa2", "BLUS30386", 0, 4);	/* the disc in the drive (v0.15) */

		d->where = 11;
		d = add(P_PS3, "Red Dead Redemption", "BLUS30418", 0, 5);	/* a disc shortcut, disc out */
		d->where = 11;
		d->shortcut = 1;
	}
	add(P_PSP, "Patapon", "UCUS98711", 211000000ULL, 3)->where = 2;		/* v0.16: a PSP image on the hard disk */
	add(P_PSP, "Lumines.iso", "ULUS10046", 120000000ULL, 0)->where = 0;
	stub_console(1, SU_APP, 0);
	ui_prepare();
	{
		/* v0.28: the opening, a frame every half second or so */
		static const float at[] = { 0.4f, 0.9f, 1.3f, 1.8f, 2.3f, 2.9f, 3.6f, 4.3f };
		unsigned k;

		for (k = 0; k < sizeof at / sizeof at[0]; k++)
		{
			char nm[32];

			intro_draw(at[k]);
			snprintf(nm, sizeof nm, "intro_%02d", (int)(at[k] * 10.0f + 0.5f));
			save(nm);
		}
	}

	ui_home_go(1);
	shot("home", 120);
	ui_pad(PAD_CROSS, 0);		/* HOME -> PS2 */
	shot("games_ps2", 300);
	ui_pad(PAD_RIGHT, 0);
	shot("ps2_moving", 304);	/* mid-glide */
	shot("ps2_next", 420);
	ui_pad(PAD_TRIANGLE, 0);
	ui_pad(PAD_CROSS, 0);		/* type "A" */
	shot("search", 480);
	ui_pad(PAD_TRIANGLE, 0);
	ui_pad(PAD_CIRCLE, 0);		/* back HOME */
	ui_home_go(3);			/* PS3 tile */
	shot("home_ps3", 600);
	ui_pad(PAD_CROSS, 0);
	shot("empty_ps3", 660);
	ui_pad(PAD_CIRCLE, 0);
	ui_home_go(0);			/* PS1 tile */
	ui_pad(PAD_CROSS, 0);
	shot("games_ps1", 720);
	next_name = "starting";
	progress(ui_chosen(), "STARTING", ((const game_t *)ui_chosen())->shown, 0.8f, "Handing over to the PS2 side");
	ui_pad(PAD_CIRCLE, 0);
	ui_home_go(4);			/* MEMORY CARD */
	for (n_card_files = 0; n_card_files + 3 < argc && n_card_files < 8; n_card_files++)
		card_files[n_card_files] = argv[3 + n_card_files];
	ui_pad(PAD_CROSS, 0);
	{
		int i, j;

		for (i = 0; i < n_cards; i++)
		{
			printf("card %s: ok %d ps1 %d saves %d free %d KB\n", cards[i].name, cards[i].ok, cards[i].ps1, cards[i].n, cards[i].free_kb);
			for (j = 0; j < cards[i].n; j++)
				printf("   %-32s %-40s %7u B %2d files icon %d nv %d %s\n", cards[i].saves[j].dir, cards[i].saves[j].title, cards[i].saves[j].bytes,
					cards[i].saves[j].nfiles, cards[i].saves[j].icon, cards[i].saves[j].nv, cards[i].saves[j].when);
		}
	}
	shot("mc", 900);
	if (getenv("MC_BENCH"))
	{
		/* v0.27: time the Memory Card screen alone (its turning 3D save icons) */
		struct timespec a, b;
		int k;

		clock_gettime(CLOCK_MONOTONIC, &a);
		for (k = 0; k < 300; k++)
		{
			frame = 900 + k;
			ui_draw();
		}
		clock_gettime(CLOCK_MONOTONIC, &b);
		printf("MC screen: %.2f ms/frame on this PC (300 frames)\n", ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6) / 300.0);
	}
	ui_pad(PAD_RIGHT, 0);
	ui_pad(PAD_RIGHT, 0);
	shot("mc_sel2", 1500);
	ui_pad(PAD_CROSS, 0);
	shot("mc_options", 1560);
	ui_pad(PAD_UP, 0);
	ui_pad(PAD_CROSS, 0);
	shot("mc_ask", 1600);
	ui_pad(PAD_CIRCLE, 0);
	ui_pad(PAD_R1, 0);
	shot("mc_card2", 1700);

	/* v0.15: five tiles with the consoles' pictures, SETTINGS, the PS2 setup question, options,
	 * the two delete questions */
	ui_pad(PAD_CIRCLE, 0);		/* MEMORY CARD -> HOME */
	ui_home_go(0);
	shot("v15_home_ps1", 1800);
	ui_home_go(1);
	shot("v15_home_ps2", 1830);
	ui_home_go(3);
	shot("v15_home_ps3", 1860);
	ui_home_go(2);
	shot("v16_home_psp", 1870);
	ui_pad(PAD_CROSS, 0);
	shot("v16_psp_list", 1880);
	ui_pad(PAD_CIRCLE, 0);
	ui_home_go(5);
	shot("v15_home_settings", 1890);
	ui_pad(PAD_CROSS, 0);
	shot("v15_settings_ready", 1900);
	chk.wm_autoplay = 1;			/* v0.22: webMAN's Auto-Play on */
	shot("v22_settings_autoplay", 1902);
	chk.wm_autoplay = 0;
	chk.wm_req = 0;				/* v0.22: webMAN without the request file (Lite) */
	shot("v22_settings_lite", 1904);
	chk.wm_req = 1;
	stub_console(1, SU_NONE, 0);
	shot("v15_settings_setup", 1910);
	ui_pad(PAD_CROSS, 0);
	shot("v15_settings_ask", 1920);
	ui_pad(PAD_CIRCLE, 0);
	stub_console(0, SU_NONE, 0);
	shot("v15_settings_cecha", 1930);
	stub_console(1, SU_NONE, 0);
	ui_pad(PAD_CIRCLE, 0);		/* SETTINGS -> HOME */
	ui_home_go(1);			/* PS2 */
	ui_pad(PAD_CROSS, 0);		/* the first PS2 open asks about the setup */
	shot("v15_home_setup_ask", 1950);
	ui_pad(PAD_CROSS, 0);		/* "Not now": into the PS2 list */
	stub_console(1, SU_APP, 0);
	ui_pad(PAD_CIRCLE, 0);
	ui_home_go(3);			/* PS3 */
	ui_pad(PAD_CROSS, 0);
	ui_pad(PAD_RIGHT, 0);
	shot("v15_ps3_disc", 2100);
	ui_pad(PAD_SQUARE, 0);
	shot("v15_ps3_disc_options", 2110);
	ui_pad(PAD_SQUARE, 0);
	ui_pad(PAD_LEFT, 0);
	shot("v15_ps3_folder", 2200);
	ui_pad(PAD_SQUARE, 0);
	shot("v15_ps3_folder_options", 2210);
	ui_pad(PAD_UP, 0);
	ui_pad(PAD_CROSS, 0);		/* Delete: main would ask game_del_plan(); here straight to the question */
	ui_ask_delete("550 files in 18 folders, 3.2 GB, from the hard disk");
	shot("v15_delete_ask1", 2220);
	ui_pad(PAD_RIGHT, 0);
	ui_pad(PAD_CROSS, 0);
	frame = 2225;
	shot("v15_delete_ask2_early", 2225);
	shot("v15_delete_ask2", 2300);
	{
		/* v0.46: SETTINGS with its Help row, every help page, and the first-start terms screen */
		char nm[40];
		int k;

		ui_pad(PAD_CIRCLE, 0);
		ui_pad(PAD_CIRCLE, 0);
		ui_pad(PAD_CIRCLE, 0);		/* out of the question and the list, to HOME */
		ui_home_go(5);
		ui_pad(PAD_CROSS, 0);
		shot("v46_settings", 2400);
		/* v0.62: the bar on a row lower down (the PSP launcher row), then back up to Help */
		for (k = 0; k < 4; k++)
			ui_pad(PAD_DOWN, 0);
		shot("v62_settings_pspl", 2401);
		for (k = 0; k < 4; k++)
			ui_pad(PAD_UP, 0);
		ui_pad(PAD_CROSS, 0);		/* Help */
		for (k = 0; k < 11; k++)
		{
			snprintf(nm, sizeof nm, "v46_help_%02d", k + 1);
			shot(nm, 2410 + k);
			ui_pad(PAD_RIGHT, 0);
		}
		frame = 2500;
		backdrop();
		terms_draw(1);
		save("v46_terms");
	}
	{
		/* v0.65: the only card sits in a slot that gives back damaged data (cardcheck.h) */
		int k;

		for (k = 0; k < 4; k++)
			ui_pad(PAD_CIRCLE, 0);
		n_games[P_PS2] = 0;
		card_state = LR_E_BADSLOT;
		card_bad_slot = 2;
		ui_home_go(1);
		shot("v65_home_badslot", 2600);
		ui_pad(PAD_CROSS, 0);
		shot("v65_ps2_badslot", 2610);
	}
	{
		/* v0.66: PS2 games from three cards at once, then a failed CF slot beside a sound SD card */
		int k;

		for (k = 0; k < 2; k++)
			ui_pad(PAD_CIRCLE, 0);
		n_games[P_PS2] = 5;
		for (k = 0; k < 4; k++)
		{
			games[P_PS2][k].where = 1;
			games[P_PS2][k].slot = (unsigned char)(2 - k % 3);
		}
		card_state = 0;
		card_bad_slot = -1;
		card_read_mask = 7;
		ui_prepare();
		ui_home_go(1);
		ui_pad(PAD_CROSS, 0);
		shot("v66_ps2_three_cards", 2700);
		ui_pad(PAD_RIGHT, 0);
		shot("v66_ps2_three_cards_next", 2800);
		card_read_mask = 2;
		card_bad_slot = 2;
		shot("v66_ps2_sd_cf_bad", 2900);
	}
	return 0;
}
