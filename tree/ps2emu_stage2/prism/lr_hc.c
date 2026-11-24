/* LR-G2 option (b): cooperative use of the EHCI controller the emulator itself is running
 * (SB_USB device 1, 0x60300000), for the one device on ROOT PORT 1 - the card reader, which
 * the emulator's driver never resets or enables.
 *
 * Facts this file is built on (re_notes/gxemu_ehci_coop_2026-11-21.md, ps2_gxemu 4.93):
 *
 *   The emulator's async schedule is   head(H) -> list heads + [its transfers] -> sentinel -> head
 *   (two list heads, 0x800 and the anchor 0x7FF: measured on the mule 2026-11-21, see ring_check)
 *     head     = *(0x17751D0)  dummy queue head with the H bit, ASYNCLISTADDR target
 *     anchor   = *(0x17751B8)  its submit inserts right after this one      (0x06EED8..0x06EF0C)
 *     sentinel = *(0x17751C8)  its completion scan walks head .. sentinel and STOPS there
 *                              without touching the sentinel or following its link (0x0719E0..0x071A68)
 *   Its unlink walk (0x06E3B8) starts at the anchor and stops at the H bit; it rewrites only
 *   the link of the queue head in front of one of its own transfers.
 *
 *   => sentinel->hlink is written by nobody but init. Our queue heads go between the sentinel
 *      and the head. The completion scan never sees them; the unlink walk can pass through
 *      them (so they live inside the emulator's own DMA region, where its bus->pointer
 *      translation holds) and never rewrites them; publishing them is ONE store to a word
 *      the emulator does not write.
 *
 *   Memory: the emulator's EHCI DMA region is 0x30000 bytes and its init touches only the
 *   first 0x240C0. We use [0x25000, 0x30000): no hypercall, and the bridge is already
 *   mapped to it. attach() refuses if that range is not all zero.
 *
 * Registers: this file READS the shared controller's registers and WRITES exactly one:
 * PORTSC1, for the port reset. USBCMD / USBSTS / USBINTR / ASYNCLISTADDR / PORTSC2 are the
 * emulator's and are never written. No transfer asks for an interrupt.
 */
#include <ps2emu/ps2emu.h>
#include <ps2emu/libc.h>
#include "lr.h"

#if defined(PS2GXEMU)
#define GX_DMA_PTRBASE	0x1774E98	/* u64: EA of the EHCI DMA region */
#define GX_DMA_BUSBASE	0x1774E90	/* u64: its bus address */
#define GX_RING_HEAD	0x17751D0
#define GX_RING_SENT	0x17751C8
#define GX_RING_ANCHOR	0x17751B8
#define GX_CUR_PORT	0x1775288
#define GX_ATTACHED	0x1775251
#else
/* ps2_emu (re_notes/ps2_emu_ehci_coop_2026-11-23.md). Its EHCI init (0x03C41C) builds the same
 * ring from the same layout: memset 0x240C0 at the fixed EA 0x30050000, head(H) = ASYNCLISTADDR
 * target, sentinel, then list heads (0x506088) and the anchor = last pool slot (0x506090) put in
 * after the head: head -> 0x506088 -> anchor -> sentinel -> head. Its completion scan (0x03B530)
 * walks head .. sentinel and stops there. Its bus addresses: bus = EA - 0x30000000 + *(0x505BC8). */
#define HW_DMA_EA	0x30050000ULL	/* EA of its EHCI DMA region (fixed) */
#define HW_DMA_DELTA	0x505BC8	/* u64: the bus address of EA 0x30000000 */
#define GX_RING_HEAD	0x5060A8
#define GX_RING_SENT	0x5060A0
#define GX_RING_ANCHOR	0x506090
#define GX_CUR_PORT	0x506168	/* -> the PORTSC register its port handler 0x03AE54 treats as current */
#define GX_ATTACHED	0x506129	/* u8 flag that handler reads; logged only */
#endif
#define GX_REGION_SZ	0x30000
#define GX_INIT_END	0x240C0

#define USBCMD		0x00
#define USBSTS		0x04
#define CONFIGFLAG	0x40
#define PORTSC1		0x44

#define P_CCS		0x00000001
#define P_PE		0x00000004
#define P_RESET		0x00000100
#define P_POWER		0x00001000
#define P_OWNER		0x00002000
#define P_RWC		0x0000002a	/* connect change, enable change, over-current change */

#define T_ACTIVE	0x00000080
#define T_HALTED	0x00000040
#define T_ERRBITS	0x00000038	/* data buffer error, babble, transaction error */
#define PID_OUT		0
#define PID_IN		1
#define PID_SETUP	2
#define LINK_T		1
#define LINK_QH		2

#define N_QH		3
#define N_TD		4		/* working qTDs; one more is the short-packet terminator */
#define TD_END		N_TD
#define TD_MAX		0x4000		/* bytes per qTD */
#define OFF_TD		0x100

u8 *lr_dma;
u8 lr_swap;	/* 1 = the bridge reverses the bytes inside each 32-bit word of DATA (see lr_usb.c) */

static u64 op, pbase, bbase;
static u32 *ring_sent;
static u32 qh_w0[N_QH], qh_w1[N_QH];
static u8 attached, broken;
static u32 n_faillog;

static u32 bus_of(const void *p)
{
	return (u32)((u64)p - pbase + bbase);
}

static u32 *QH(int q)
{
	return (u32 *)(lr_dma + 0x40 * q);
}

static u32 *TD(int i)
{
	return (u32 *)(lr_dma + OFF_TD + 0x20 * i);
}

static int in_region(const u32 *p)
{
	return (u64)p >= pbase && (u64)p + 0x40 <= pbase + GX_REGION_SZ && !((u64)p & 0x1f);
}

static u32 vrd(const u32 *p)
{
	return *(volatile const u32 *)p;
}

static void vwr(u32 *p, u32 v)
{
	*(volatile u32 *)p = v;
}

/* ---------------------------------------------------------------- geometry */

#if defined(PS2HWEMU)
static int geom(int quiet)
{
	u64 cap;

	if (!lr_emu_usb_started())
		return LR_E_STATE;

	cap = *(u64 *)(u64)GX_EHCI_CAPBASE_PTR;
	op = *(u64 *)(u64)GX_EHCI_OPBASE_PTR;
	pbase = HW_DMA_EA;
	bbase = *(u64 *)(u64)HW_DMA_DELTA + (HW_DMA_EA - 0x30000000ULL);

	/* PERIODICLISTBASE: its init writes the bus address of the region's first byte there
	 * (0x03CA18), the one independent check that bbase is right */
	if (cap != HW_EA_EHCI(1) || op != cap + (lr_rd(cap, 0) & 0xff) || !*(u64 *)(u64)HW_DMA_DELTA ||
		(bbase & 0xfff) || ((bbase + GX_REGION_SZ) >> 32) || lr_rd(op, 0x14) != (u32)bbase)
	{
		if (!quiet)
			lr_log("hc: ABORT geometry: cap %lx op %lx ptr %lx bus %lx per %x\n", (unsigned long)cap, (unsigned long)op,
				(unsigned long)pbase, (unsigned long)bbase, op ? lr_rd(op, 0x14) : 0);
		return LR_E_STATE;
	}

	return 0;
}
#else
static int geom(int quiet)
{
	lr_gx_usbdev *d = LR_DEVTAB;
	lr_gx_dma *m;
	u64 cap;

	if (!lr_emu_usb_started())
		return LR_E_STATE;

	cap = *(u64 *)(u64)GX_EHCI_CAPBASE_PTR;
	op = *(u64 *)(u64)GX_EHCI_OPBASE_PTR;
	pbase = *(u64 *)(u64)GX_DMA_PTRBASE;
	bbase = *(u64 *)(u64)GX_DMA_BUSBASE;
	m = (lr_gx_dma *)d[1].dma_desc;

	if (cap != d[1].ea_ehci || op != cap + (lr_rd(cap, 0) & 0xff) || d[1].n_dma < 2 || !m || !pbase || (pbase & 0xfff) ||
		(bbase & 0xfff) || ((bbase + GX_REGION_SZ) >> 32) || m[1].lpar != pbase || m[1].bus != bbase || m[1].size < GX_REGION_SZ)
	{
		if (!quiet)
			lr_log("hc: ABORT geometry: cap %lx op %lx ptr %lx bus %lx size %lx\n", (unsigned long)cap, (unsigned long)op,
				(unsigned long)pbase, (unsigned long)bbase, (unsigned long)(m ? m[1].size : 0));
		return LR_E_STATE;
	}

	return 0;
}
#endif

/* The emulator's ring must have the shape this file was written for. As measured on the mule
 * on 2026-11-21 (bench run R1) it is
 *
 *     head(H) -> list head 0x800 -> [bulk transfers] -> anchor (list head 0x7FF)
 *             -> [control transfers] -> sentinel -> head
 *
 * What we rely on is checked by walking it from the head: every node is a queue head inside
 * the part of the region the emulator's init built, only the head has the H bit, the anchor
 * comes before the sentinel, and THE RING CLOSES RIGHT AFTER THE SENTINEL (or, once we are
 * attached, the sentinel links to our first queue head). No node may use our device address.
 * Returns LR_E_STATE (-5) for a wrong shape, LR_E_XACT (-4) for an address clash. */
static int ring_check(u32 **psent, u32 **phead)
{
	u32 *head = *(u32 **)(u64)GX_RING_HEAD, *sent = *(u32 **)(u64)GX_RING_SENT, *anc = *(u32 **)(u64)GX_RING_ANCHOR;
	u32 *p;
	int n, seen_anc = 0, clash = 0;

	if (!in_region(head) || !in_region(sent) || !in_region(anc) || head == sent || sent == anc || head == anc)
		return LR_E_STATE;
	if ((u64)head >= pbase + GX_INIT_END || (u64)sent >= pbase + GX_INIT_END || (u64)anc >= pbase + GX_INIT_END)
		return LR_E_STATE;
	if (!(vrd(head + 1) & 0x8000) || (vrd(sent + 1) & 0x8000) || (vrd(anc + 1) & 0x8000))
		return LR_E_STATE;

	p = head;
	for (n = 0;; n++)
	{
		u32 link = vrd(p);
		u32 *nx;

		if (n > 2200 || (link & 7) != LINK_QH || (link & ~0x1fu) < (u32)bbase)
			return LR_E_STATE;
		nx = (u32 *)(pbase + ((link & ~0x1fu) - (u32)bbase));
		if (!in_region(nx))
			return LR_E_STATE;

		if (p == sent)
		{
			if (!seen_anc || (nx != head && !(attached && nx == QH(0))))
				return LR_E_STATE;
			break;
		}

		if (nx == head || (u64)nx >= pbase + GX_INIT_END || (vrd(nx + 1) & 0x8000))
			return LR_E_STATE;
		if (nx == anc)
			seen_anc = 1;
		if ((vrd(nx + 1) & 0x7f) == LR_USB_ADDR)
			clash = 1;
		p = nx;
	}

	if (clash)
		return LR_E_XACT;

	*psent = sent;
	*phead = head;
	return 0;
}

/* ------------------------------------------------------------------- probe */
/* Read-only. Logs what attach() will rely on, as seen on the real machine. */

void lr_hc_probe(const char *tag)
{
	u32 *sent = NULL, *head = NULL, *p;
	u32 nz = 0, sum = 0x811c9dc5, i;
	u64 *w;
	int r, n;

	if (geom(1))
	{
		lr_log("hc %s: emulator EHCI not up or geometry unexpected\n", tag);
		return;
	}

	r = ring_check(&sent, &head);
	lr_log("hc %s: ring %d head %lx sent %lx anchor %lx cur-port %lx attached %d\n", tag, r,
		(unsigned long)*(u64 *)(u64)GX_RING_HEAD, (unsigned long)*(u64 *)(u64)GX_RING_SENT,
		(unsigned long)*(u64 *)(u64)GX_RING_ANCHOR, (unsigned long)*(u64 *)(u64)GX_CUR_PORT, *(u8 *)(u64)GX_ATTACHED);
	lr_log("hc %s: cmd %08x sts %08x p1 %08x p2 %08x\n", tag, lr_rd(op, USBCMD), lr_rd(op, USBSTS), lr_rd(op, PORTSC1),
		lr_rd(op, PORTSC1 + 4));

	p = *(u32 **)(u64)GX_RING_HEAD;
	for (n = 0; n < 24 && in_region(p); n++)
	{
		u32 link = vrd(p);

		lr_log(" qh %08x link %08x ep %08x cur %08x next %08x tok %08x\n", bus_of(p), link, vrd(p + 1), vrd(p + 3), vrd(p + 4),
			vrd(p + 6));
		if ((link & 7) != LINK_QH || (link & ~0x1fu) < (u32)bbase)
			break;
		p = (u32 *)(pbase + ((link & ~0x1fu) - (u32)bbase));
		if (p == *(u32 **)(u64)GX_RING_HEAD)
			break;
	}

	/* the spare range we intend to use: must be zero and must stay as we left it */
	w = (u64 *)(pbase + LR_BLOCK_OFF);
	for (i = 0; i < LR_DMA_SZ / 8; i++)
	{
		if (w[i])
			nz++;
		sum = (sum ^ (u32)w[i] ^ (u32)(w[i] >> 32)) * 0x01000193;
	}
	lr_log("hc %s: spare %x..%x nonzero words %u sum %08x (ours %d)\n", tag, LR_BLOCK_OFF, LR_BLOCK_OFF + LR_DMA_SZ, nz, sum, attached);
}

/* ------------------------------------------------------------------ attach */

static void qh_idle(u32 *h)
{
	vwr(h + 4, LINK_T);
	vwr(h + 5, LINK_T);
	vwr(h + 6, 0);
	vwr(h + 3, 0);
}

int lr_hc_attach(void)
{
	u32 *sent, *head, cmd, sts, link;
	u8 *t;
	int q, i;

	if (attached)
		return broken ? LR_E_STATE : 0;

	if (geom(0))
		return LR_E_STATE;
	q = ring_check(&sent, &head);
	if (q)
	{
		lr_log("hc: ABORT %s\n", q == LR_E_XACT ? "a device of the emulator's already uses our address" :
			"the emulator's async ring is not the shape expected");
		return LR_E_STATE;
	}

	cmd = lr_rd(op, USBCMD);
	sts = lr_rd(op, USBSTS);
	if ((cmd & 0x23) != 0x21 || (sts & 0x1000) || !(lr_rd(op, CONFIGFLAG) & 1))
	{
		lr_log("hc: ABORT controller not running its async schedule: cmd %08x sts %08x\n", cmd, sts);
		return LR_E_STATE;
	}

	link = vrd(sent);
	if (link != (bus_of(head) | LINK_QH))
	{
		lr_log("hc: ABORT sentinel link %08x is not the head %08x\n", link, bus_of(head));
		return LR_E_STATE;
	}

	t = (u8 *)(pbase + LR_BLOCK_OFF);
	for (i = 0; i < LR_DMA_SZ / 8; i++)
	{
		if (((u64 *)t)[i])
		{
			lr_log("hc: ABORT spare range is in use: %lx at +%x\n", (unsigned long)((u64 *)t)[i], LR_BLOCK_OFF + i * 8);
			return LR_E_STATE;
		}
	}

	lr_dma = t;
	ring_sent = sent;

	for (q = 0; q < N_QH; q++)
	{
		u32 *h = QH(q);

		qh_w0[q] = (q == N_QH - 1) ? link : (bus_of(QH(q + 1)) | LINK_QH);
		qh_w1[q] = (64u << 16) | (2u << 12) | (q == LR_Q_CTRL ? 0x4000u : 0);
		vwr(h + 0, qh_w0[q]);
		vwr(h + 1, qh_w1[q]);
		vwr(h + 2, 1u << 30);
		qh_idle(h);
	}

	vwr(TD(TD_END) + 0, LINK_T);
	vwr(TD(TD_END) + 1, LINK_T);
	vwr(TD(TD_END) + 2, 0);

	__asm__ volatile("sync");
	vwr(sent, bus_of(QH(0)) | LINK_QH);	/* the one publishing store */
	__asm__ volatile("sync");

	attached = 1;
	lr_log("hc: attached. sentinel %08x -> ours %08x..%08x -> head %08x\n", bus_of(sent), bus_of(QH(0)), bus_of(QH(N_QH - 1)),
		bus_of(head));

	return 0;
}

/* Everything we rely on is still as we left it. */
static int alive(void)
{
	u32 cmd, p;
	int q;

	if (!attached || broken)
		return LR_E_STATE;

	cmd = lr_rd(op, USBCMD);
	if ((cmd & 0x23) != 0x21 || (lr_rd(op, USBSTS) & 0x1000))
	{
		lr_log("hc: controller stopped (cmd %08x); giving up for this session\n", cmd);
		broken = 1;
		return LR_E_STATE;
	}

	if (vrd(ring_sent) != (bus_of(QH(0)) | LINK_QH))
	{
		lr_log("hc: our link was replaced (%08x); giving up for this session\n", vrd(ring_sent));
		broken = 1;
		return LR_E_STATE;
	}

	for (q = 0; q < N_QH; q++)
	{
		if (vrd(QH(q)) != qh_w0[q] || vrd(QH(q) + 1) != qh_w1[q])
		{
			lr_log("hc: queue head %d was overwritten (%08x %08x); giving up for this session\n", q, vrd(QH(q)), vrd(QH(q) + 1));
			broken = 1;
			return LR_E_STATE;
		}
	}

	p = lr_rd(op, PORTSC1);
	if ((p & (P_CCS | P_PE | P_RESET | P_OWNER)) != (P_CCS | P_PE))
		return LR_E_XACT;	/* port dropped: the caller resets and re-enumerates */

	return 0;
}

/* -------------------------------------------------------------------- port */

int lr_hc_port_reset(void)
{
	u32 v;
	int i, q;

	if (!attached || broken)
		return LR_E_STATE;

	v = lr_rd(op, PORTSC1);
	lr_log("hc: p1 before reset %08x\n", v);
	if (!(v & P_CCS) || (v & P_OWNER) || !(v & P_POWER))
	{
		lr_log("hc: nothing usable on root port 1\n");
		return LR_E_NOMEDIUM;
	}

	lr_wr(op, PORTSC1, (v & ~(P_RWC | P_PE)) | P_RESET);
	lr_mdelay(50);

	/* Any transfer of ours that was still pending has now failed against a dead port. */
	for (q = 0; q < N_QH; q++)
	{
		u32 *h = QH(q);

		if (vrd(h + 6) & T_ACTIVE)
			lr_mdelay(5);
		if (vrd(h + 6) & T_ACTIVE)
		{
			lr_log("hc: queue head %d still active under reset; giving up for this session\n", q);
			broken = 1;
		}
		else
			qh_idle(h);
	}

	/* End the reset. The emulator's port-change handler acknowledges change bits with a
	 * read-modify-write of this register; if it ran while reset was set it re-arms it, so
	 * clear until it reads back clear. */
	for (i = 0; i < 20; i++)
	{
		v = lr_rd(op, PORTSC1);
		if (!(v & P_RESET))
			break;
		lr_wr(op, PORTSC1, v & ~(P_RWC | P_RESET));
		lr_mdelay(2);
	}

	lr_mdelay(20);		/* reset recovery */
	v = lr_rd(op, PORTSC1);
	lr_log("hc: p1 after reset %08x (%d)\n", v, i);

	if (broken)
		return LR_E_STATE;
	if ((v & (P_CCS | P_PE | P_RESET)) != (P_CCS | P_PE))
	{
		lr_log("hc: port 1 did not come up enabled (not a high-speed device?)\n");
		return LR_E_XACT;
	}

	return 0;
}

/* ------------------------------------------------------------------- pipes */

void lr_hc_pipe(int q, u32 addr, u32 ep, u32 mps)
{
	u32 *h = QH(q);

	if (!attached || broken)
		return;

	qh_idle(h);
	qh_w1[q] = (mps << 16) | (2u << 12) | (ep << 8) | addr | (q == LR_Q_CTRL ? 0x4000u : 0);
	vwr(h + 1, qh_w1[q]);
	__asm__ volatile("sync");
	lr_udelay(500);		/* a traversal that read the old word has finished by now */
}

void lr_hc_toggle0(int q)
{
	if (attached && !broken && !(vrd(QH(q) + 6) & T_ACTIVE))
		qh_idle(QH(q));
}

/* --------------------------------------------------------------- transfers */

static void td_fill(int i, u32 pid, u32 dt, u32 bus, u32 len, int last)
{
	u32 *t = TD(i);
	int k;

	vwr(t + 0, last ? LINK_T : bus_of(TD(i + 1)));
	vwr(t + 1, bus_of(TD(TD_END)));		/* short packet: stop here, do not run the rest */
	vwr(t + 3, bus);
	for (k = 1; k < 5; k++)
		vwr(t + 3 + k, (bus & ~0xfffu) + 0x1000u * (u32)k);
	vwr(t + 2, (dt << 31) | (len << 16) | (3u << 10) | (pid << 8) | T_ACTIVE);
}

/* Run TD(0..n-1) on queue head q. 0 or LR_E_*. */
static int run(int q, int n, u32 timeout_ms)
{
	u32 *h = QH(q);
	u64 end;
	int r, i;

	r = alive();
	if (r)
		return r;

	if ((vrd(h + 6) & T_ACTIVE) || !(vrd(h + 4) & LINK_T))
	{
		lr_log("hc: queue head %d busy (tok %08x next %08x)\n", q, vrd(h + 6), vrd(h + 4));
		return LR_E_STATE;
	}
	if (vrd(h + 6) & T_HALTED)
		qh_idle(h);

	vwr(h + 5, LINK_T);
	__asm__ volatile("sync");
	vwr(h + 4, bus_of(TD(0)));		/* hand the chain to the controller */
	__asm__ volatile("sync");

	end = lr_tb() + (u64)timeout_ms * LR_TB_PER_MS;
	for (;;)
	{
		r = 1;
		for (i = 0; i < n; i++)
		{
			u32 t = vrd(TD(i) + 2);

			if (t & T_HALTED)
			{
				r = (t & T_ERRBITS) ? LR_E_XACT : LR_E_STALL;
				break;
			}
			if (t & T_ACTIVE)
				break;
			if ((t >> 16) & 0x7fff)
			{
				/* bytes left over: a short packet ended the chain here, on the last qTD
				 * as much as on an earlier one */
				r = LR_E_SHORT;
				break;
			}
			if (i == n - 1)
			{
				r = 0;
				break;
			}
		}
		if (r <= 0)
			break;
		if ((s64)(lr_tb() - end) > 0)
		{
			r = LR_E_TIMEOUT;
			break;
		}
	}

	if (r < 0 && n_faillog < 16)
	{
		/* the raw evidence, before anything is cleaned up */
		n_faillog++;
		lr_log("hc: q%d fail %d n %d td %08x %08x %08x %08x ov cur %08x next %08x tok %08x sts %08x p1 %08x\n", q, r, n,
			vrd(TD(0) + 2), vrd(TD(1) + 2), vrd(TD(2) + 2), vrd(TD(3) + 2), vrd(h + 3), vrd(h + 4), vrd(h + 6),
			lr_rd(op, USBSTS), lr_rd(op, PORTSC1));
	}

	if (r == LR_E_STALL || r == LR_E_XACT)
		qh_idle(h);	/* halted: the controller no longer runs it, clear the overlay */
	else if (r == LR_E_TIMEOUT)
	{
		/* A pending transfer cannot be withdrawn from a live schedule without the doorbell,
		 * which is the emulator's. Resetting the port makes it fail, which ends it. The
		 * device is back at address 0 afterwards: every caller re-enumerates on an error. */
		lr_log("hc: timeout on queue head %d, tok %08x\n", q, vrd(h + 6));
		lr_hc_port_reset();
	}

	return r;
}

/* Reverse the bytes inside each 32-bit word, in place (len is rounded up to a whole word). */
static void swap_words(u8 *p, u32 len)
{
	u32 *w = (u32 *)p, n = (len + 3) >> 2;

	while (n--)
	{
		u32 v = *w;

		*w++ = (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
	}
}

/* Control transfer on endpoint 0. Direction comes from setup[0]; an IN data stage must be
 * answered in full. */
int lr_hc_ctrl(const u8 *setup, void *data, int len)
{
	u8 *s, *d;
	int in = setup[0] & 0x80, n = 0, r;

	if (!attached || broken || len < 0 || len > LR_CTL_MAX)
		return LR_E_STATE;

	s = lr_dma + LR_OFF_SETUP;
	d = lr_dma + LR_OFF_CTLDATA;
	memcpy(s, (void *)setup, 8);
	memset(d, 0, ((u32)len + 3) & ~3u);
	if (len && !in)
		memcpy(d, data, len);
	if (lr_swap)
	{
		swap_words(s, 8);
		if (len && !in)
			swap_words(d, (u32)len);
	}

	td_fill(n++, PID_SETUP, 0, bus_of(s), 8, 0);
	if (len)
		td_fill(n++, in ? PID_IN : PID_OUT, 1, bus_of(d), (u32)len, 0);
	td_fill(n++, (len && in) ? PID_OUT : PID_IN, 1, bus_of(d), 0, 1);

	r = run(LR_Q_CTRL, n, 1000);
	if (r < 0 && len && in)
		lr_log("hc: ctrl data area as received: %02x %02x %02x %02x %02x %02x %02x %02x\n", d[0], d[1], d[2], d[3], d[4], d[5],
			d[6], d[7]);
	if (r == 0 && len && in)
	{
		if (lr_swap)
			swap_words(d, (u32)len);
		memcpy(data, d, len);
	}

	return r;
}

/* Bulk transfer of len bytes at lr_dma + off. The data toggle lives in the queue head. */
int lr_hc_bulk(int q, u32 off, u32 len, u32 timeout_ms)
{
	u32 pid = (q == LR_Q_IN) ? PID_IN : PID_OUT, done = 0;
	int n = 0, r;

	if (!attached || broken || !len || len > N_TD * TD_MAX || off + len > LR_DMA_SZ || (off & 3))
		return LR_E_STATE;

	if (lr_swap && pid == PID_OUT)
		swap_words(lr_dma + off, len);

	while (done < len)
	{
		u32 chunk = len - done;

		if (chunk > TD_MAX)
			chunk = TD_MAX;
		td_fill(n, pid, 0, bus_of(lr_dma + off + done), chunk, done + chunk == len);
		done += chunk;
		n++;
	}

	r = run(q, n, timeout_ms);

	/* OUT: put the caller's bytes back as they were. IN: bring what arrived into byte order. */
	if (lr_swap && (pid == PID_OUT || r == 0 || r == LR_E_SHORT))
		swap_words(lr_dma + off, len);

	return r;
}
