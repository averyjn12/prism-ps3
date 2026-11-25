/* Stand-ins for the PC renderer (render.sh): what screens.c reads from games.c (v0.63 card_dev, v0.66
 * game_where_name) and cardcheck.c (v0.65/v0.66) that needs the console. */
#include <ppu-types.h>
#include "storage.h"
#include "games.h"

u64 card_dev = ST_DEV_SD;
int card_bad_slot = -1;
int card_read_mask = 2;

const char *slot_name(int s)
{
	static const char *const nm[3] = { "Memory Stick", "SD", "CompactFlash" };

	return s >= 0 && s < 3 ? nm[s] : "card";
}

const char *game_where_name(const game_t *g)
{
	static const char *const s[3] = { "MS", "SD", "CF" };

	return g->where == 1 && g->slot < 3 ? s[g->slot] : where_name(g->where);
}
