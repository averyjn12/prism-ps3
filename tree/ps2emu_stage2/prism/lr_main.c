/* Entry points called from gxemu/main.c. */
#include <ps2emu/ps2emu.h>
#include <ps2emu/libc.h>
#include "lr.h"

#define LATE_POLL	3000	/* ATAPI commands after which the "late" full snapshot is taken */

#if PRISM_LR == 1
#include <ps2emu/io.h>
/* LR-APP capture (2026-11-22, probe build only): copy the emulator's boot-parameter globals
 * into a file the PC made beforehand, so the 0x4F0-byte block the XMB hands over with
 * sys_sm_shutdown(0x8202) can be read out (vsh.self 0xD4E24). ps2_gxemu rebuilds that block
 * from the struct at 0x6E6300 (builder 0x584CC, TOC-0x7908; block area struct+0x2718);
 * 0x2300 is the other boot global (TOC-0x7FC8). Writes only inside CAP_FILE, in place. */
#define CAP_FILE	"/tmp/prism_capture.bin"	/* = /dev_hdd0/tmp/prism_capture.bin, >= 64 KB */
#define CAP_A		0x6E6300ULL
#define CAP_B		0x2300ULL
#define CAP_LEN		0x3000
#define CAP_LEN_A	0x3600		/* struct+0x2F00 holds the SM packet with the block: +0x2F18..+0x3408 */

static void lr_capture(int slot)
{
	int fd = ufs_open(0, CAP_FILE);

	if (fd < 0)
	{
		lr_log("cap: no %s\n", CAP_FILE);
		return;
	}
	if (lr_file_size(fd) >= 0x10000)
	{
		ufs_write(fd, (u64)slot * 0x8000, (void *)CAP_A, CAP_LEN_A);
		ufs_write(fd, (u64)slot * 0x8000 + CAP_LEN_A, (void *)CAP_B, CAP_LEN);
		lr_log("cap: slot %d written (%x %x bytes, %x %x bytes)\n", slot, (u32)CAP_A, CAP_LEN_A, (u32)CAP_B, CAP_LEN);
	}
	else
		lr_log("cap: %s is short\n", CAP_FILE);
	ufs_close(fd);
}
#else
#define lr_capture(slot)	((void)0)
#endif
#define MAX_CHG		20	/* port-change lines kept */

#if defined(PS2HWEMU)
/* A01 field kit: is the 64 KB stage2 slot (0x021f0000) left alone by ps2_emu? Everything of ours
 * that never changes after start-up - code and constants (__slot_from .. __reloc_start), TOC and
 * OPD (from __toc_start) - is summed at start and again at each checkpoint: ~62 of the 64 KB.
 * Not covered: data and bss (they change by design), .start (run once), and the plain .text of
 * main.o / patch.o before __slot_from (hwemu/ldscript.ld): Cobra copies the emulator's original
 * instructions into main.o's hook trampolines AFTER lr_init (seen by the PC harness 2026-11-24). */
extern u32 __slot_from[], __reloc_start[], __toc_start[], __self_end[];
static u32 slot0;

static u32 slot_add(u32 s, const u32 *p, const u32 *e)
{
	while (p < e)
		s = ((s << 1) | (s >> 31)) ^ *p++;
	return s;
}

static void slot_check(const char *when)
{
	u32 s = slot_add(slot_add(0, __slot_from, __reloc_start), __toc_start, __self_end);

	if (!slot0)
		slot0 = s;
	lr_log("slot %s: %08x %s\n", when, s, s == slot0 ? "intact" : "CHANGED");
}
#else
#define slot_check(when)	((void)0)
#endif

static u8 lr_ok, lr_g3_done;
static u32 lr_polls, lr_sig, lr_nchg;

static void lr_g3_maybe(const char *when)
{
#if PRISM_LR == 3
	if (lr_g3_done)
		return;
	if (!lr_emu_usb_started())
	{
		lr_log("g3: emulator usb not started at %s, deferring\n", when);
		return;
	}

	lr_g3_done = 1;
	lr_log("g3: running at %s\n", when);
	lr_log_flush();
	lr_ehci_run();
	lr_log_flush();
	lr_snapshot("after-g3");
#else
	(void)when;
	(void)lr_g3_done;
#endif
}

/* ---- the emulator's disc commands, counted ---- */
#define OPS	14
#define RUN	40

static struct
{
	u16 op[OPS];
	u32 n[OPS];
	u8 run[RUN];		/* the first commands after the last mark, in order */
	u8 n_run, n_op, logged;
	const char *why;
} at = { .why = "start" };
static u8 at_watch;
static u64 at_t0;

void lr_atapi_op(u32 op)
{
	int i;

	for (i = 0; i < at.n_op && at.op[i] != op; i++)
		;
	if (i == at.n_op)
	{
		if (at.n_op == OPS)
			i = OPS - 1;	/* anything more is lumped into the last slot */
		else
			at.n_op++;
		at.op[i] = (u16)op;
	}
	at.n[i]++;

	if (at.n_run < RUN)
		at.run[at.n_run++] = (u8)(op > 0xff ? 0xff : op);

	/* after a mid-game swap: every command goes to the log file at once (console run D1 froze
	 * before the next once-a-minute record, and the log could not say what the emulator did) */
	if (at_watch)
	{
		at_watch--;
		lr_log("after swap +%u ms: %x\n", (u32)((lr_tb() - at_t0) / LR_TB_PER_MS), op);
		lr_log_flush();
	}
}

void lr_atapi_watch(u32 n)
{
	at_watch = (u8)n;
	at_t0 = lr_tb();
}

int lr_atapi_due(void)
{
	return at.n_run == RUN && !at.logged;
}

void lr_atapi_mark(const char *why)
{
	at.why = why;
	at.n_run = 0;
	at.logged = 0;
}

void lr_atapi_log(const char *tag)
{
	char line[180];
	int i, n = 0;

	for (i = 0; i < at.n_op; i++)
		n += lr_fmt(line + n, (int)sizeof(line) - n, " %x:%u", at.op[i], at.n[i]);
	lr_log("atapi %s:%s\n", tag, n ? line : " none");

	if (!at.logged && at.n_run)
	{
		at.logged = at.n_run == RUN;
		for (i = 0, n = 0; i < at.n_run; i++)
			n += lr_fmt(line + n, (int)sizeof(line) - n, " %02x", at.run[i]);
		lr_log("atapi after %s:%s\n", at.why, line);
	}
}

void lr_init(void)
{
	lr_log("PRISM LR %s gate %d tb %lx\n", LR_VERSION, PRISM_LR, (unsigned long)lr_tb());
	lr_log_flush();		/* proof that this build ran, even if something below hangs */

	lr_ok = (u8)lr_sig_check();
	if (!lr_ok)
	{
		lr_log(LR_EMU_NAME " signature mismatch: probe disabled\n");
		lr_net_try();
		lr_log_flush();
		return;
	}

	lr_net_try();
#if PRISM_LR < 5
	lr_repo_dump();
#endif
	lr_snapshot("t0-entry");
	slot_check("t0");
	lr_capture(0);
	lr_sig = lr_port_sig();
	lr_log_flush();
	lr_g3_maybe("entry");
#if PRISM_LR >= 5
	lr_hc_probe("t0");
	lr_log_flush();
	lr_card_init();
#endif
	lr_log_flush();
}

void lr_poll(void)
{
	u32 s;

	if (!lr_ok)
		return;

	lr_polls++;
	if (lr_atapi_due())
	{
		lr_atapi_log("run");
		lr_log_flush();
	}

	if (lr_polls == 1)
	{
		lr_net_try();
		lr_snapshot("t1-first-atapi");
		slot_check("t1");
		lr_capture(1);
#if PRISM_LR >= 5
		lr_hc_probe("t1");
#endif
		lr_sig = lr_port_sig();
		lr_g3_maybe("first-atapi");
		lr_log_flush();
		return;
	}

	if (lr_polls == LATE_POLL)
	{
		lr_snapshot("t2-late");
		slot_check("t2");
#if PRISM_LR >= 5
		lr_hc_probe("t2");
		lr_card_stats("t2");
#endif
		lr_sig = lr_port_sig();
		lr_log_flush();
		return;
	}

	if (lr_polls == 64 || lr_polls == 512)
	{
		lr_net_try();
#if PRISM_LR >= 5
		lr_card_stats(lr_polls == 64 ? "p64" : "p512");
		lr_log_flush();
#endif
	}

#if PRISM_LR >= 5
	/* early lines too: a small title makes only a few dozen ATAPI commands in all */
	if (lr_polls == 4 || lr_polls == 16 || (lr_polls & 0x7fff) == 0)
	{
		slot_check("run");
		lr_card_stats(lr_polls == 4 ? "p4" : lr_polls == 16 ? "p16" : "run");
		lr_log_flush();
	}
#endif

#if PRISM_LR >= 5
	if (lr_polls > 16)
		lr_card_tick();
	lr_swap_tick();
#endif

	s = lr_port_sig();
	if (s != lr_sig && lr_nchg < MAX_CHG)
	{
		char tag[12];

		lr_sig = s;
		lr_nchg++;
		tag[0] = '#';
		tag[1] = (char)('0' + (lr_nchg / 10) % 10);
		tag[2] = (char)('0' + lr_nchg % 10);
		tag[3] = 0;
		lr_log("poll %d\n", (int)lr_polls);
		lr_port_line(tag);
		lr_g3_maybe("port-change");
		lr_log_flush();
	}
}
