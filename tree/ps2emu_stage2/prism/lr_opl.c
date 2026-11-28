/* OPL for Prism, gate OPL-G1 (2026-11-28): the OPL helper's card layer. Built INSTEAD of lr_card.c
 * and lr_fs.c when the Makefile is given LR_OPL=1 (lr/opl/DESIGN.md). The emulator wiring
 * (gxemu/main.c, hwemu/main.c) and the USB / controller code are the same objects as in the
 * card-game helper.
 *
 * Trigger: the same one-shot file (LR_CFG_FILE) or the carrier's marker at LR_MARK_OFF, with
 *     PRISMLR1
 *     mode=opl
 *     lun=N            (optional; the slot Prism picked)
 * Any other mode, or no trigger: nothing is touched and the carrier is served as it is.
 *
 * In mode=opl the carrier keeps running from the hard disk and the disc is reported full size
 * (lr_iso_take returns 2, as the launcher does). Disc sectors:
 *     0 .. LR_OPL_W              the carrier, as Cobra serves it
 *     LR_OPL_W .. +LR_OPL_N      the card window: 2048 bytes = four 512-byte card blocks, from
 *                                window k's start (k x LR_OPL_N x 2048 bytes into the card)
 *     LR_OPL_C .. +LR_OPL_K      command sectors: reading C + k selects window k and returns the
 *                                header (layout in lr.h, little-endian, the PS2's byte order)
 *     LR_OPL_T .. +LR_OPL_TN     tuning sectors (G1S, 11-28): reading T + x sets tuning x (card
 *                                blocks per command, read-ahead cache on/off) and returns the header
 *     anything else              zeroes
 * Each header read ends a "step": the header carries that step's call count and times (lr.h).
 * The card is only ever READ. A read error re-enumerates the card (as lr_card.c); if that fails,
 * the window serves zeroes and the header's state says why. */
#include <ps2emu/ps2emu.h>
#include <ps2emu/libc.h>
#include <ps2emu/io.h>
#include <ps2emu/symbols.h>
#include "lr.h"

#if !defined(LR_OPL)
#error "lr_opl.c is the OPL helper's card layer: build it with LR_OPL=1"
#endif

#define CFG_SZ		512
#define MAX_RECOVER	3
#define LR_STAT_MS	60000ULL

static struct
{
	u8 valid, opl, keep;
	int lun;
} cfg;

static int carrier_fd = -1;
static u8 opl_on, card_ready;
static int card_r = LR_E_STATE;
static u32 cur_k;
static u32 saved_blocks;

static struct
{
	u32 reads, cmds, errs, recov, max_us;
	u64 bytes, tb;
} st;

/* G1S (11-28): one step's numbers, reported and cleared by each header read (lr.h, version 2) */
static struct
{
	u32 calls, sectors, min_sec, max_sec, card_cmds, hit, fetched, n_short, n_long, pf_started, pf_used, pf_failed;
	u64 busy, card, first, last, copy, g_short, g_long, pf_wait;
} sp;
static u8 sp_clear;

/* tuning (lr.h LR_OPL_T) and the read-ahead cache: card blocks [c_lba, c_lba + c_n) sit at
 * lr_dma + LR_OPL_BUF_OFF while c_ok. Card byte offsets, so a window switch does not touch it. */
static u32 tune, ra = LR_OPL_RA_DEF;
static u8 cache_on, c_ok, synth, fused, pf_on, pf_pend;
static u32 pf_lba, pf_n;
static u8 c_pf;	/* the cache line came from a prefetch and no call has used it yet */
static u32 c_lba, c_n;

static int starts(const char *s, const char *key)
{
	while (*key)
	{
		if (*s++ != *key++)
			return 0;
	}
	return 1;
}

static int cfg_read(int fd, u64 off, u64 size)
{
	char buf[CFG_SZ + 1];
	char *p, *end;

	if (size > CFG_SZ)
		size = CFG_SZ;
	memset(buf, 0, sizeof(buf));
	if (size < 16 || ufs_read(fd, off, buf, size) <= 0 || !starts(buf, "PRISMLR1"))
		return 0;
	buf[size] = 0;

	for (p = buf; *p; p = end)
	{
		for (end = p; *end && *end != '\n'; end++)
			;
		if (*end)
			*end++ = 0;
		if (starts(p, "mode="))
			cfg.opl = starts(p + 5, "opl");
		else if (starts(p, "lun=") && p[4] >= '0' && p[4] <= '7')
			cfg.lun = p[4] - '0';
		else if (starts(p, "keep=1"))
			cfg.keep = 1;
	}
	return 1;
}

static void cfg_load(void)
{
	const char *src = "trigger";
	int fd = ufs_open(0, LR_CFG_FILE);

	cfg.valid = cfg.opl = cfg.keep = 0;
	cfg.lun = -1;
	if (fd >= 0)
	{
		cfg.valid = (u8)cfg_read(fd, 0, lr_file_size(fd));
		if (cfg.valid && !cfg.keep)
		{
			char x = 'x';

			ufs_write(fd, 0, &x, 1);
		}
		ufs_close(fd);
	}
	if (!cfg.valid && carrier_fd >= 0)
	{
		src = "carrier";
		cfg.valid = (u8)cfg_read(carrier_fd, LR_MARK_OFF, CFG_SZ);
	}
	if (cfg.valid)
		lr_log("cfg: %s opl %d lun %d keep %d\n", src, cfg.opl, cfg.lun, cfg.keep);
	else
		lr_log("cfg: none, nothing touched\n");
}

static int bring_up(int first)
{
	int r;

	if (!lr_emu_usb_started())
	{
		lr_log("opl: the emulator's USB is not up at stage2 entry\n");
		return LR_E_STATE;
	}
	if (first)
	{
		r = lr_hc_attach();
		if (r)
			return r;
	}
	r = lr_hc_port_reset();
	if (r)
		return r;
	r = lr_usb_enumerate();
	if (r)
		return r;
	return lr_msc_select(first ? cfg.lun : lr_lun, first ? LR_MSC_WAIT_FIRST : LR_MSC_WAIT_AGAIN);
}

void lr_carrier(int fd)
{
	carrier_fd = fd;
}

void lr_card_init(void)
{
	cfg_load();
	lr_log_flush();
	if (!cfg.valid || !cfg.opl)
		return;

	opl_on = 1;
	card_r = bring_up(1);
	card_ready = (card_r == 0);
	saved_blocks = lr_blk_count;
	lr_log("opl: card %d, %u blocks; window at %u (+%u), commands at %u\n", card_r, lr_blk_count, LR_OPL_W, LR_OPL_N,
		LR_OPL_C);
	lr_log_flush();
}

int lr_iso_take(u64 *size, int *is_cd, int *is_raw)
{
	(void)size;
	(void)is_cd;
	(void)is_raw;
	return opl_on ? 2 : 0;
}

int lr_event(const u8 *cmd, u8 *out)
{
	(void)cmd;
	(void)out;
	return 0;
}

void lr_swap_tick(void)
{
}

static void put32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static void header(u8 *p)
{
	u64 blocks = card_ready ? (u64)lr_blk_count : 0;
	const char *v = LR_VERSION;
	int i;

	memset(p, 0, 2048);
	memcpy(p, LR_OPL_MAGIC, 8);
	put32(p + 8, LR_OPL_HDR_VER);
	put32(p + 12, (u32)(card_ready ? 0 : card_r));
	put32(p + 16, (u32)blocks);
	put32(p + 20, (u32)(blocks >> 32));
	put32(p + 24, 512);
	put32(p + 28, LR_OPL_W);
	put32(p + 32, LR_OPL_N);
	put32(p + 36, LR_OPL_C);
	put32(p + 40, (u32)((blocks + (u64)LR_OPL_N * 4 - 1) / ((u64)LR_OPL_N * 4)));
	put32(p + 44, cur_k);
	put32(p + 48, st.reads);
	put32(p + 52, st.errs);
	for (i = 0; v[i] && i < 31; i++)
		p[64 + i] = (u8)v[i];
	put32(p + 96, sp.calls);
	put32(p + 100, sp.sectors);
	put32(p + 104, sp.min_sec);
	put32(p + 108, sp.max_sec);
	put32(p + 112, (u32)(sp.busy * 1000 / LR_TB_PER_MS));
	put32(p + 116, (u32)(sp.card * 1000 / LR_TB_PER_MS));
	put32(p + 120, sp.calls ? (u32)((sp.last - sp.first) * 1000 / LR_TB_PER_MS) : 0);
	put32(p + 124, sp.card_cmds);
	put32(p + 128, sp.hit);
	put32(p + 132, sp.fetched);
	put32(p + 136, tune);
	put32(p + 140, ra);
	put32(p + 144, cache_on);
	put32(p + 148, synth);
	put32(p + 152, fused);
	put32(p + 156, (u32)(sp.copy * 1000 / LR_TB_PER_MS));
	put32(p + 160, sp.n_short);
	put32(p + 164, (u32)(sp.g_short * 1000 / LR_TB_PER_MS));
	put32(p + 168, sp.n_long);
	put32(p + 172, (u32)(sp.g_long * 1000 / LR_TB_PER_MS));
	put32(p + 176, pf_on);
	put32(p + 180, sp.pf_started);
	put32(p + 184, sp.pf_used);
	put32(p + 188, (u32)(sp.pf_wait * 1000 / LR_TB_PER_MS));
	put32(p + 192, sp.pf_failed);
	sp_clear = 1;	/* the step ends with the call that carries this header */
}

/* G1U prefetch: the next line's card command, left running (lr_blk_read_start). pf_finish() ends it:
 * on success the line becomes the cache line; on any failure the card is brought up again (the
 * plain path then reads the blocks, with its own recovery). Everything that uses the bus, or the
 * card buffer, calls pf_finish() first. */
static int bring_up(int first);

/* After a failed prefetch: prefetching stops until the tuning is set again (one failure costs one reset, never
 * one per call), and the card is brought up again with card_fill's retries. A reset that fails, or brings back a
 * different medium, marks the card DEAD so the header says so: G1U on the E01 (11-28) had a failed reset leave
 * the block count at 0 with the header still "ready", and the window served zeroes nobody could tell from data. */
static void pf_recover(void)
{
	int r = 1, tries;

	pf_on = 0;
	lr_log("opl: prefetch off until the tuning is set again\n");
	for (tries = 0; r && tries < MAX_RECOVER; tries++)
	{
		r = bring_up(0);
		if (r == 0 && lr_blk_count != saved_blocks)
		{
			lr_log("opl: a different medium came back (%u blocks, was %u)\n", lr_blk_count, saved_blocks);
			r = LR_E_STATE;
			break;
		}
	}
	if (r)
	{
		card_ready = 0;
		card_r = r;
		lr_log("opl: card DEAD after a failed prefetch (%d); the window serves zeroes\n", r);
		lr_log_flush();
	}
	else
		st.recov++;
}

static void pf_begin(u32 lba)
{
	u32 n = ra;
	int r;

	if ((u64)lba + n > lr_blk_count)
		n = lr_blk_count - lba;
	c_ok = 0;
	r = lr_blk_read_start(lba, n, LR_OPL_BUF_OFF);
	if (r)
	{
		sp.pf_failed++;
		st.errs++;
		if (sp.pf_failed <= 4)
			lr_log("opl: prefetch at block %x did not start (%d)\n", lba, r);
		if (lr_hc_in2_busy())
			lr_blk_read_poll(1);
		pf_recover();
		return;
	}
	pf_pend = 1;
	pf_lba = lba;
	pf_n = n;
	sp.pf_started++;
	sp.card_cmds++;
}

static void pf_finish(void)
{
	u64 t0;
	int r;

	if (!pf_pend)
		return;
	t0 = lr_tb();
	pf_pend = 0;
	r = lr_blk_read_poll(1);
	sp.pf_wait += lr_tb() - t0;
	if (r)
	{
		sp.pf_failed++;
		st.errs++;
		if (sp.pf_failed <= 4)
			lr_log("opl: prefetch at block %x failed %d; bringing the card up again\n", pf_lba, r);
		c_ok = 0;
		pf_recover();
		return;
	}
	sp.fetched += pf_n * 512;
	c_lba = pf_lba;
	c_n = pf_n;
	c_ok = 1;
	c_pf = 1;
}

/* blocks [lba, lba + n) into the card buffer, with re-enumeration on an error. 0 or LR_E_*;
 * on an error the card is marked dead (the window serves zeroes from then on). */
static int card_fill(u32 lba, u32 n)
{
	u64 t0 = lr_tb();
	int r, tries;

	pf_finish();
	c_ok = 0;	/* the buffer is being overwritten (and bring-up uses it too) */
	sp.card_cmds++;
	r = lr_blk_read(lba, n, LR_OPL_BUF_OFF);
	for (tries = 0; r && tries < MAX_RECOVER; tries++)
	{
		st.errs++;
		if (st.errs <= 8)
			lr_log("opl: card read at block %x failed %d, recovering (%d)\n", lba, r, tries + 1);
		r = bring_up(0);
		if (r == 0 && lr_blk_count != saved_blocks)
		{
			lr_log("opl: a different medium came back (%u blocks, was %u)\n", lr_blk_count, saved_blocks);
			r = LR_E_STATE;
			break;
		}
		if (r == 0)
		{
			st.recov++;
			sp.card_cmds++;
			r = lr_blk_read(lba, n, LR_OPL_BUF_OFF);
		}
	}
	if (r)
	{
		card_ready = 0;
		card_r = r;
		lr_log("opl: card DEAD at block %x (%d); the window serves zeroes\n", lba, r);
		lr_log_flush();
	}
	else
	{
		sp.fetched += n * 512;
		if (cache_on)
		{
			c_lba = lba;
			c_n = n;
			c_ok = 1;
		}
	}
	sp.card += lr_tb() - t0;
	return r;
}

static void set_tune(u32 x)
{
	pf_finish();
	tune = x;
	cache_on = (u8)(x & 1);
	synth = (u8)((x >> 7) & 1);
	fused = (u8)((x >> 8) & 1);
	pf_on = (u8)(cache_on && ((x >> 9) & 1));
	lr_hc_rawin_off = fused ? LR_OPL_BUF_OFF : 0;
	ra = ((x >> 1) & 63) ? ((x >> 1) & 63) * 8 : LR_OPL_RA_DEF;
	if (ra > LR_OPL_BUF_SZ / 512)
		ra = LR_OPL_BUF_SZ / 512;
	c_ok = 0;
}

/* n bytes of the card buffer from at to dst. With the fused copy on, the buffer holds the bytes as the
 * bridge put them (each 32-bit word reversed when lr_swap): undo that here, 8 bytes a load from the
 * uncached DMA memory when everything is 8-aligned, else a byte at a time. */
static void out_copy(u8 *dst, u32 at, u32 n)
{
	const u8 *src = lr_dma + LR_OPL_BUF_OFF;
	u64 t0 = lr_tb();

	if (!fused || !lr_swap)
		memcpy(dst, src + at, n);
	else if (!((at | n | (u32)(u64)dst) & 7))
	{
		const volatile u64 *s = (const volatile u64 *)(src + at);
		u64 *d = (u64 *)dst;
		u32 i;

		for (i = 0; i < n / 8; i++)
		{
			u64 x = s[i];

			d[i] = ((u64)__builtin_bswap32((u32)(x >> 32)) << 32) | __builtin_bswap32((u32)x);
		}
	}
	else
	{
		u32 i;

		for (i = 0; i < n; i++)
			dst[i] = src[(at + i) ^ 3];
	}
	sp.copy += lr_tb() - t0;
}

/* len bytes from card byte offset coff. Served from the cache where it holds them; otherwise one
 * card command of ra blocks (cache on) or of just the blocks asked, at most ra (cache off). Past
 * the card's end, and on a dead card, zeroes: never what the buffer held before. */
static int card_read(u64 coff, u8 *buf, u32 len)
{
	u64 total = (u64)saved_blocks * 512;	/* the card's size as first found: a reset that changes it marks it dead */
	int r = 0;

	while (len)
	{
		u32 take;

		if (pf_pend)
			pf_finish();
		if (!card_ready || coff >= total)
		{
			memset(buf, 0, len);
			break;
		}
		if (c_ok && coff >= (u64)c_lba * 512 && coff < ((u64)c_lba + c_n) * 512)
		{
			u32 at = (u32)(coff - (u64)c_lba * 512);

			take = c_n * 512 - at;
			if (take > len)
				take = len;
			out_copy(buf, at, take);
			sp.hit += take;
			if (c_pf)
			{
				sp.pf_used++;
				c_pf = 0;
			}
		}
		else
		{
			u32 lba = (u32)(coff >> 9), skip = (u32)(coff & 511), n;

			n = (cache_on && !pf_on) ? ra : (skip + len + 511) >> 9;
			if (n > ra)
				n = ra;
			if ((u64)lba + n > lr_blk_count)
				n = lr_blk_count - lba;
			r = card_fill(lba, n);
			if (r)
			{
				memset(buf, 0, len);
				break;
			}
			take = n * 512 - skip;	/* short of len only at the card's end: the next pass zeroes */
			if (take > len)
				take = len;
			out_copy(buf, skip, take);
		}
		buf += take;
		coff += take;
		len -= take;
	}
	if (!r && pf_on && card_ready && c_ok && coff == ((u64)c_lba + c_n) * 512 && coff < total)
		pf_begin((u32)(coff >> 9));
	return r;
}

/* Replaces ufs_read(iso_fd, ...) in Cobra's common.c, as lr_card.c's does. */
int lr_iso_read(int fd, u64 off, void *buf, u64 len)
{
	u8 *b = (u8 *)buf;
	u64 left = len;
	u64 t0, t1;
	u32 us;

	if (!opl_on)
		return ufs_read(fd, off, buf, len);

	t0 = lr_tb();
	while (left)
	{
		u64 sec = off / 2048, end;
		u32 take;

		if (sec < LR_OPL_W)
		{
			/* the carrier, up to the window's start */
			int got;

			end = (u64)LR_OPL_W * 2048;
			take = (u32)((end - off < left) ? end - off : left);
			got = ufs_read(fd, off, b, take);
			if (got < 0)
				got = 0;
			if ((u32)got < take)
				memset(b + got, 0, take - (u32)got);	/* past the carrier's end: zeroes, never stale bytes */
		}
		else if (sec < (u64)LR_OPL_W + LR_OPL_N)
		{
			end = ((u64)LR_OPL_W + LR_OPL_N) * 2048;
			take = (u32)((end - off < left) ? end - off : left);
			st.reads++;
			st.bytes += take;
			if (synth)
				memset(b, 0, take);	/* G1T: the emulator's own floor, no card */
			else
				card_read((u64)cur_k * LR_OPL_N * 2048 + (off - (u64)LR_OPL_W * 2048), b, take);
		}
		else if ((sec >= LR_OPL_C && sec < (u64)LR_OPL_C + LR_OPL_K) || (sec >= LR_OPL_T && sec < (u64)LR_OPL_T + LR_OPL_TN))
		{
			/* one command sector: select window k (or set tuning x), answer the header. Not on
			 * the stack: this runs on the emulator's thread, whose stack size is not ours to know. */
			static u8 one[2048];
			u32 at = (u32)(off & 2047);

			take = 2048 - at;
			if (take > left)
				take = (u32)left;
			if (sec >= LR_OPL_T)
				set_tune((u32)(sec - LR_OPL_T));
			else
				cur_k = (u32)(sec - LR_OPL_C);
			st.cmds++;
			header(one);
			memcpy(b, one + at, take);
			if (st.cmds <= 4)
				lr_log("opl: command sector %u: window %u tuning %u\n", (u32)sec, cur_k, tune);
		}
		else
		{
			end = (sec + 1) * 2048;
			take = (u32)((end - off < left) ? end - off : left);
			memset(b, 0, take);
		}
		b += take;
		off += take;
		left -= take;
	}
	t1 = lr_tb();
	if (sp_clear)
	{
		/* this call carried a header: the next step starts after it */
		memset(&sp, 0, sizeof(sp));
		sp_clear = 0;
	}
	else
	{
		u32 n = (u32)(len / 2048);

		if (!sp.calls || n < sp.min_sec)
			sp.min_sec = n;
		if (n > sp.max_sec)
			sp.max_sec = n;
		if (!sp.calls)
			sp.first = t0;
		else if (t0 - sp.last < 2 * LR_TB_PER_MS)
		{
			sp.n_short++;
			sp.g_short += t0 - sp.last;
		}
		else
		{
			sp.n_long++;
			sp.g_long += t0 - sp.last;
		}
		sp.calls++;
		sp.sectors += n;
		sp.busy += t1 - t0;
		sp.last = t1;
	}
	t0 = t1 - t0;
	st.tb += t0;
	us = (u32)(t0 * 1000 / LR_TB_PER_MS);
	if (us > st.max_us)
		st.max_us = us;
	return (int)len;
}

void lr_card_stats(const char *tag)
{
	if (!opl_on)
		return;
	lr_log("opl: %s reads %u cmds %u bytes %lx errs %u recov %u max %u us total %u ms window %u ready %d\n", tag, st.reads,
		st.cmds, (unsigned long)st.bytes, st.errs, st.recov, st.max_us, (u32)(st.tb / LR_TB_PER_MS), cur_k, card_ready);
}

void lr_card_tick(void)
{
	static u64 next;
	u64 now;

	if (!opl_on)
		return;
	now = lr_tb();
	if (now < next)
		return;
	next = now + LR_STAT_MS * LR_TB_PER_MS;
	lr_log_tail("opl: tail reads %u cmds %u bytes %lx errs %u recov %u max %u us total %u ms window %u ready %d\n", st.reads,
		st.cmds, (unsigned long)st.bytes, st.errs, st.recov, st.max_us, (u32)(st.tb / LR_TB_PER_MS), cur_k, card_ready);
}
