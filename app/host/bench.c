/* Time the PS2 carousel the way the console draws it (same ui.c / screens.c), on the PC.
 *   bench <art file> [frames]      gprof with -pg for the split by function */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gfx.h"
#include "games.h"
#include "screens.h"
#include "vmc.h"

u32 *fb;
game_t games[N_PLAT][MAX_GAMES];
int n_games[N_PLAT];
int card_state;
int gfx_init(void) { fb = calloc(CW * CH, 4); return 0; }
void gfx_end(void) {}
void gfx_present(void) {}
void games_scan(void) {}
const char *where_name(int w) { static const char *const n[] = { "SD", "SD", "HDD", "USB 0", "USB 1", "USB 2", "USB 3", "USB 4", "USB 5", "USB 6", "USB 7", "DISC" }; return w >= 0 && w < 12 ? n[w] : "?"; }
int vmc_load_all(void) { return 0; }

static void add(int p, const char *name, const char *id, unsigned long long size, int kind)
{
	game_t *g = &games[p][n_games[p]++];

	memset(g, 0, sizeof(*g));
	snprintf(g->name, sizeof g->name, "%s", name);
	snprintf(g->id, sizeof g->id, "%s", id);
	g->size = size;
	g->kind = (unsigned char)kind;
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	int n = argc > 2 ? atoi(argv[2]) : 600, i, which;
	double t0, t;

	gfx_init();
	art_load_from(argv[1]);
	add(P_PS2, "Metal Gear Solid 2 - Substance (USA).iso", "SLUS_205.54", 6436749312ULL, 0);
	add(P_PS2, "GTASA.ISO", "SLUS_209.46", 4501209088ULL, 0);
	add(P_PS2, "Crazy Taxi (USA).bin", "", 700000000ULL, 2);
	add(P_PS2, "Dynasty Warriors 4 (Disc 1).iso", "SLUS_206.53", 4000000000ULL, 0);
	add(P_PS2, "Tiny Tees.iso", "SLUS_999.01", 1376256ULL, 0);
	add(P_PS1, "Metal Gear Solid (USA) (Disc 1) (Rev 1).bin", "SLUS_005.94", 705614112ULL, 1);
	ui_prepare();
	screen = SCR_HOME;
	for (which = 0; which < 2; which++)
	{
		screen = SCR_HOME;
		frame = 0;
		ui_draw();
		ui_pad(which ? PAD_RIGHT : 0, 0);
		ui_pad(PAD_CROSS, 0);		/* into PS2 (which 0) / PS1 (which 1) */
		t0 = now();
		for (i = 0; i < n; i++)
		{
			frame = (unsigned)i;
			if (i % 20 == 0)
				ui_pad((i / 20) % 8 < 4 ? PAD_RIGHT : PAD_LEFT, 0);
			ui_draw();
		}
		t = now() - t0;
		printf("%s list, cycling every 20 frames: %.2f ms/frame on this PC (%d frames)\n", which ? "PS1" : "PS2", t * 1000.0 / n, n);
		ui_pad(PAD_CIRCLE, 0);
	}
	screen = SCR_HOME;
	t0 = now();
	for (i = 0; i < n; i++)
	{
		frame = (unsigned)i;
		ui_draw();
	}
	printf("HOME, idle: %.2f ms/frame\n", (now() - t0) * 1000.0 / n);
	return 0;
}
