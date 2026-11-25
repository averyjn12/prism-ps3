/* v0.65: the card-slot check on the console. What it judges: cardcheck_core.h; who calls it: cardcheck.h. Read-only. */
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include "cardcheck.h"
#include "cardcheck_core.h"
#include "fsport.h"
#include "games.h"
#include "storage.h"

int fsport_raw(u32 lba, u32 n, void *buf);	/* fsport_ps3.c */

int slot_cc[3] = { -1, -1, -1 };
char slot_cc_why[3][120];
int card_bad_slot = -1;
int card_read_mask;

static const char *const nm[3] = { "Memory Stick", "SD", "CompactFlash" };
static const char *const dir[3] = { "/dev_ms", "/dev_sd", "/dev_cf" };

int card_lun_for(int s)
{
	int k;

	for (k = 0; k < s && k < 3; k++)
		if (slot_cc[k] >= 0)
			return s;
	return -1;
}

u64 slot_storage(int s)
{
	return s == 0 ? ST_DEV_MS : s == 2 ? ST_DEV_CF : ST_DEV_SD;
}

const char *slot_name(int s)
{
	return s >= 0 && s < 3 ? nm[s] : "card";
}

int slot_of_dev(u64 dev)
{
	return dev == ST_DEV_MS ? 0 : dev == ST_DEV_SD ? 1 : dev == ST_DEV_CF ? 2 : -1;
}

int slot_of_path(const char *p)
{
	int s;

	for (s = 0; s < 3 && p; s++)
		if (!strncmp(p, dir[s], 7) && (p[7] == 0 || p[7] == '/'))
			return s;
	return -1;
}

int slot_of_game(const void *gv)
{
	const game_t *g = (const game_t *)gv;

	if (g->where == 1)
		return g->slot < 3 ? g->slot : -1;	/* v0.66: the game's own slot */
	return slot_of_path(g->path);
}

int slot_bad(int s)
{
	return s >= 0 && s < 3 && (slot_cc[s] == CC_BAD || slot_cc[s] == CC_IOERR);
}

int slot_bad_msg(int s, const char *doing, char *msg, int n)
{
	if (!slot_bad(s))
		return 0;
	snprintf(msg, (size_t)n, "%s: the %s slot is giving back %s from this card (most likely the adaptor). Nothing was written. Try the card in another slot.",
		doing, nm[s], slot_cc[s] == CC_BAD ? "damaged data" : "read errors");
	return 1;
}

static int rd_raw(void *ctx, uint32_t lba, uint32_t n, uint8_t *buf)
{
	int t;

	(void)ctx;
	for (t = 0; t < 3; t++)
		if (fsport_raw(lba, n, buf) == 0)
			return 0;
	return -1;
}

static u64 tbase(void)
{
	u64 t;

	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

int cardcheck_attached(int s, FILE *lg)
{
	static u8 *a, *b;
	u64 t0 = tbase();
	int judged = 0, r;

	if (s < 0 || s > 2)
		return CC_THIN;
	if (!a)
		a = (u8 *)memalign(128, CC_BUF);
	if (!b)
		b = (u8 *)memalign(128, CC_BUF);
	if (!a || !b)
	{
		slot_cc[s] = CC_THIN;
		snprintf(slot_cc_why[s], sizeof slot_cc_why[s], "no memory for the check");
		return CC_THIN;
	}
	r = cc_card(rd_raw, NULL, lr_blk_count, a, b, slot_cc_why[s], sizeof slot_cc_why[s], &judged);
	slot_cc[s] = r;
	if (lg)
		fprintf(lg, "slot %s: check %s, %d test(s), %llu ms: %s\n", nm[s],
			r == CC_OK ? "OK" : r == CC_BAD ? "BAD DATA" : r == CC_IOERR ? "READ ERRORS" : "nothing to judge by", judged,
			(unsigned long long)((tbase() - t0) / 79800), slot_cc_why[s]);
	return r;
}
