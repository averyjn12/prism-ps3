/* LR-G3: own SB_USB device 0's EHCI controller (the one gxemu leaves untouched), reset one
 * high-speed port, read the 18-byte device descriptor at address 0, then give everything back.
 *
 * Rules this file keeps:
 *   - It refuses to touch the controller unless it is HALTED with CONFIGFLAG 0 and is not the
 *     controller the emulator's own EHCI driver uses (GX_EHCI_CAPBASE_PTR).
 *   - USBINTR stays 0. The emulator has an interrupt handler plugged on this controller's line
 *     but its handler acknowledges a DIFFERENT controller, so one interrupt would never clear.
 *   - Everything is polled against the timebase with a timeout; nothing can wait forever.
 *   - On every exit path the controller is halted, CONFIGFLAG is cleared (ports go back to the
 *     OHCI companion the emulator drives) and our DMA region is unmapped.
 *
 * Register and descriptor byte order: both big-endian on this bridge. gxemu's own EHCI driver
 * stores native words into its queue heads (0x07132C: stw 0x40 -> overlay token) and builds
 * the SETUP token as 0x00080E80 with a plain stw (0x06F810), so descriptors are native too.
 */
#include <ps2emu/ps2emu.h>
#include <ps2emu/libc.h>
#include <ps2emu/memory.h>
#include <lv1/lv1.h>
#include <lv1/device.h>
#include "lr.h"

/* capability / operational registers */
#define HCSPARAMS	0x04
#define HCCPARAMS	0x08
#define USBCMD		0x00
#define USBSTS		0x04
#define USBINTR		0x08
#define CTRLDSSEG	0x10
#define ASYNCLISTADDR	0x18
#define CONFIGFLAG	0x40
#define PORTSC(n)	(0x44 + 4 * (n))

#define CMD_RUN		0x00000001
#define CMD_RESET	0x00000002
#define CMD_ASE		0x00000020
#define CMD_ITC8	0x00080000
#define STS_HALTED	0x00001000
#define STS_ASS		0x00008000

#define P_CCS		0x00000001
#define P_CSC		0x00000002
#define P_PE		0x00000004
#define P_PEC		0x00000008
#define P_OCC		0x00000020
#define P_RESET		0x00000100
#define P_LINE		0x00000c00
#define P_POWER		0x00001000
#define P_OWNER		0x00002000
#define P_RWC		(P_CSC | P_PEC | P_OCC)

#define QTD_ACTIVE	0x00000080
#define QTD_HALTED	0x00000040
#define QTD_CERR3	0x00000c00
#define QTD_PID_OUT	0x00000000
#define QTD_PID_IN	0x00000100
#define QTD_PID_SETUP	0x00000200
#define QTD_TOGGLE	0x80000000
#define LINK_T		0x00000001
#define LINK_QH		0x00000002

/* layout inside our one DMA page; every structure is 32-byte aligned */
#define OFF_QH		0x000
#define OFF_QTD_SETUP	0x080
#define OFF_QTD_DATA	0x0c0
#define OFF_QTD_STATUS	0x100
#define OFF_SETUP	0x140
#define OFF_DATA	0x180
#define DESC_LEN	18

static u8 lr_page_store[0x2000];	/* holds one page-aligned 4 KB page wherever we are loaded */

static u64 cap, op;
static int nports;

static int wait_bits(u32 off, u32 mask, u32 want, u32 timeout_ms)
{
	u64 end = lr_tb() + (u64)timeout_ms * LR_TB_PER_MS;

	while ((lr_rd(op, off) & mask) != want)
	{
		if ((s64)(lr_tb() - end) > 0)
			return -1;
	}

	return 0;
}

static void halt_and_release(void)
{
	lr_wr(op, USBCMD, lr_rd(op, USBCMD) & ~(CMD_RUN | CMD_ASE));
	if (wait_bits(USBSTS, STS_HALTED, STS_HALTED, 50))
		lr_log("g3: WARNING controller did not halt, sts %08x\n", lr_rd(op, USBSTS));
	lr_wr(op, CONFIGFLAG, 0);
	lr_log("g3: released. cmd %08x sts %08x cfg %08x\n", lr_rd(op, USBCMD), lr_rd(op, USBSTS), lr_rd(op, CONFIGFLAG));
}

/* returns 0 and the page + its bus address, trying 4 KB then 64 KB io pages */
static int dma_get(int bus_id, int dev_id, u8 **ppage, u64 *pbus, u64 *plen, void **pmalloc)
{
	u8 *page = (u8 *)(((u64)lr_page_store + 0xfff) & ~0xfffULL);
	u64 bus = 0;
	s64 r;

	*pmalloc = NULL;

	r = lv1_allocate_device_dma_region(bus_id, dev_id, 0x1000, 12, 0, &bus);
	lr_log("g3: dma alloc 4k -> %ld bus %lx\n", (long)r, (unsigned long)bus);
	if (r == 0)
	{
		r = lv1_map_device_dma_region(bus_id, dev_id, (u64)page, bus, 0x1000, 0xf800000000000000UL);
		lr_log("g3: dma map lpar %lx -> %ld\n", (unsigned long)page, (long)r);
		if (r == 0)
		{
			*ppage = page;
			*pbus = bus;
			*plen = 0x1000;
			return 0;
		}
		lv1_free_device_dma_region(bus_id, dev_id, bus);
	}

	/* second shape: one 64 KB io page out of the emulator heap */
	void *m = malloc(0x20000);

	lr_log("g3: malloc 128k -> %lx\n", (unsigned long)m);
	if (!m)
		return -1;

	page = (u8 *)(((u64)m + 0xffff) & ~0xffffULL);
	r = lv1_allocate_device_dma_region(bus_id, dev_id, 0x10000, 16, 0, &bus);
	lr_log("g3: dma alloc 64k -> %ld bus %lx\n", (long)r, (unsigned long)bus);
	if (r == 0)
	{
		r = lv1_map_device_dma_region(bus_id, dev_id, (u64)page, bus, 0x10000, 0xf800000000000000UL);
		lr_log("g3: dma map lpar %lx -> %ld\n", (unsigned long)page, (long)r);
		if (r == 0)
		{
			*ppage = page;
			*pbus = bus;
			*plen = 0x10000;
			*pmalloc = m;
			return 0;
		}
		lv1_free_device_dma_region(bus_id, dev_id, bus);
	}

	free(m);
	return -1;
}

static void dma_put(int bus_id, int dev_id, u64 bus, u64 len, void *m)
{
	s64 r1 = lv1_unmap_device_dma_region(bus_id, dev_id, bus, len);
	s64 r2 = lv1_free_device_dma_region(bus_id, dev_id, bus);

	lr_log("g3: dma unmap %ld free %ld\n", (long)r1, (long)r2);
	if (m)
		free(m);
}

/* Reset one port. Returns 1 if a high-speed device is enabled on it afterwards. */
static int port_reset(int n)
{
	u32 v = lr_rd(op, PORTSC(n));
	int i;

	lr_wr(op, PORTSC(n), (v & ~(P_RWC | P_PE)) | P_RESET);
	lr_mdelay(50);
	v = lr_rd(op, PORTSC(n));
	lr_wr(op, PORTSC(n), v & ~(P_RWC | P_RESET));

	for (i = 0; i < 20; i++)
	{
		lr_mdelay(1);
		if (!(lr_rd(op, PORTSC(n)) & P_RESET))
			break;
	}

	v = lr_rd(op, PORTSC(n));
	lr_log("g3: p%d after reset %08x (%d ms)\n", n + 1, v, i + 1);

	return (v & (P_CCS | P_PE | P_RESET)) == (P_CCS | P_PE);
}

static void port_to_companion(int n)
{
	u32 v = lr_rd(op, PORTSC(n));

	lr_wr(op, PORTSC(n), (v & ~(P_RWC | P_PE)) | P_OWNER);
}

static void w32(u8 *page, u32 off, u32 v)
{
	*(u32 *)(page + off) = v;
}

static u32 r32(u8 *page, u32 off)
{
	return *(volatile u32 *)(page + off);
}

/* GET_DESCRIPTOR(DEVICE), 18 bytes, device address 0, endpoint 0, high speed. */
static int get_device_descriptor(u8 *page, u32 bus)
{
	static const u8 setup[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, DESC_LEN, 0x00 };
	u32 qh = bus + OFF_QH, t_setup = bus + OFF_QTD_SETUP, t_data = bus + OFF_QTD_DATA, t_status = bus + OFF_QTD_STATUS;
	u64 end;
	u32 tok;
	int i, ret = -1;

	memset(page, 0, 0x200);
	memcpy(page + OFF_SETUP, (void *)setup, 8);

	/* queue head: links to itself, head of the reclamation list, EP0, 64-byte packets,
	 * high speed, data toggle taken from the qTDs */
	w32(page, OFF_QH + 0x00, qh | LINK_QH);
	w32(page, OFF_QH + 0x04, (64 << 16) | 0x8000 | 0x4000 | (2 << 12));
	w32(page, OFF_QH + 0x08, 1u << 30);
	w32(page, OFF_QH + 0x0c, 0);
	w32(page, OFF_QH + 0x10, t_setup);
	w32(page, OFF_QH + 0x14, LINK_T);
	w32(page, OFF_QH + 0x18, 0);

	w32(page, OFF_QTD_SETUP + 0x00, t_data);
	w32(page, OFF_QTD_SETUP + 0x04, LINK_T);
	w32(page, OFF_QTD_SETUP + 0x08, (8u << 16) | QTD_CERR3 | QTD_PID_SETUP | QTD_ACTIVE);
	w32(page, OFF_QTD_SETUP + 0x0c, bus + OFF_SETUP);

	w32(page, OFF_QTD_DATA + 0x00, t_status);
	w32(page, OFF_QTD_DATA + 0x04, t_status);
	w32(page, OFF_QTD_DATA + 0x08, QTD_TOGGLE | ((u32)DESC_LEN << 16) | QTD_CERR3 | QTD_PID_IN | QTD_ACTIVE);
	w32(page, OFF_QTD_DATA + 0x0c, bus + OFF_DATA);

	w32(page, OFF_QTD_STATUS + 0x00, LINK_T);
	w32(page, OFF_QTD_STATUS + 0x04, LINK_T);
	w32(page, OFF_QTD_STATUS + 0x08, QTD_TOGGLE | QTD_CERR3 | QTD_PID_OUT | QTD_ACTIVE);
	w32(page, OFF_QTD_STATUS + 0x0c, 0);

	__asm__ volatile("sync");

	lr_wr(op, ASYNCLISTADDR, qh);
	lr_wr(op, USBCMD, lr_rd(op, USBCMD) | CMD_ASE);
	if (wait_bits(USBSTS, STS_ASS, STS_ASS, 20))
	{
		lr_log("g3: async schedule did not start, sts %08x\n", lr_rd(op, USBSTS));
		goto out;
	}

	end = lr_tb() + 1000 * LR_TB_PER_MS;
	for (;;)
	{
		tok = r32(page, OFF_QTD_STATUS + 0x08);
		if (!(tok & QTD_ACTIVE))
		{
			ret = 0;
			break;
		}
		if ((r32(page, OFF_QTD_SETUP + 0x08) | r32(page, OFF_QTD_DATA + 0x08) | r32(page, OFF_QH + 0x18)) & QTD_HALTED)
		{
			ret = -2;
			break;
		}
		if ((s64)(lr_tb() - end) > 0)
		{
			ret = -3;
			break;
		}
	}

out:
	lr_wr(op, USBCMD, lr_rd(op, USBCMD) & ~CMD_ASE);
	wait_bits(USBSTS, STS_ASS, 0, 20);

	lr_log("g3: xfer %d tok setup %08x data %08x status %08x qh %08x sts %08x\n", ret,
		r32(page, OFF_QTD_SETUP + 0x08), r32(page, OFF_QTD_DATA + 0x08), r32(page, OFF_QTD_STATUS + 0x08),
		r32(page, OFF_QH + 0x18), lr_rd(op, USBSTS));

	if (ret == 0)
	{
		u8 *d = page + OFF_DATA;
		int got = DESC_LEN - (int)((r32(page, OFF_QTD_DATA + 0x08) >> 16) & 0x7fff);
		char hex[3 * DESC_LEN + 1];

		for (i = 0; i < DESC_LEN; i++)
		{
			static const char dg[] = "0123456789abcdef";

			hex[3 * i] = dg[d[i] >> 4];
			hex[3 * i + 1] = dg[d[i] & 15];
			hex[3 * i + 2] = ' ';
		}
		hex[3 * DESC_LEN] = 0;

		lr_log("g3: DESC %d bytes: %s\n", got, hex);
		lr_log("g3: VID %02x%02x PID %02x%02x bcdUSB %02x%02x mps0 %d\n", d[9], d[8], d[11], d[10], d[3], d[2], d[7]);
		if (got != DESC_LEN || d[0] != 18 || d[1] != 1)
			ret = -4;
	}

	return ret;
}

void lr_ehci_run(void)
{
	lr_gx_usbdev *dev = LR_DEVTAB;
	int bus_id = 0, dev_id = 0, i, found = 0, pass = 0;
	u8 *page = NULL;
	u64 bus = 0, len = 0;
	void *m = NULL;
	u32 w0, hcs, sts, cfg;

	cap = dev[0].ea_ehci;
	w0 = lr_rd(cap, 0);
	hcs = lr_rd(cap, HCSPARAMS);
	op = cap + (w0 & 0xff);
	nports = hcs & 0xf;
	if (nports > 8)
		nports = 8;

	sts = lr_rd(op, USBSTS);
	cfg = lr_rd(op, CONFIGFLAG);
	lr_log("g3: E0 cap %lx op %lx nports %d ppc %d cmd %08x sts %08x cfg %08x hcc %08x\n", (unsigned long)cap,
		(unsigned long)op, nports, (hcs >> 4) & 1, lr_rd(op, USBCMD), sts, cfg, lr_rd(cap, HCCPARAMS));

	/* ownership guards: all three must hold or we do not write a single register */
	if (*(u64 *)(u64)GX_EHCI_CAPBASE_PTR == cap)
	{
		lr_log("g3: ABORT the emulator's EHCI driver is on this controller\n");
		return;
	}
	if (!(sts & STS_HALTED) || (lr_rd(op, USBCMD) & CMD_RUN))
	{
		lr_log("g3: ABORT controller is running, somebody owns it\n");
		return;
	}
	if (cfg & 1)
	{
		lr_log("g3: ABORT CONFIGFLAG already 1, somebody routed its ports\n");
		return;
	}

	if (find_device_by_type(DEV_TYPE_USB, 0, &bus_id, &dev_id, NULL))
	{
		lr_log("g3: ABORT no SB_USB device 0 in the repository\n");
		return;
	}
	lr_log("g3: usb dev0 bus %d dev %d\n", bus_id, dev_id);

	if (dma_get(bus_id, dev_id, &page, &bus, &len, &m))
	{
		lr_log("g3: FAIL no DMA mapping (see codes above); controller untouched\n");
		return;
	}
	if (bus >> 32)
	{
		lr_log("g3: FAIL bus address above 4 GB\n");
		dma_put(bus_id, dev_id, bus, len, m);
		return;
	}

	lr_log("g3: taking controller 0\n");
	lr_log_flush();

	/* --- from here on we own the controller --- */
	lr_wr(op, USBCMD, CMD_RESET);
	if (wait_bits(USBCMD, CMD_RESET, 0, 250))
	{
		lr_log("g3: FAIL reset did not complete, cmd %08x\n", lr_rd(op, USBCMD));
		goto release;
	}

	lr_wr(op, USBINTR, 0);
	lr_wr(op, CTRLDSSEG, 0);
	lr_wr(op, USBCMD, CMD_ITC8 | CMD_RUN);
	if (wait_bits(USBSTS, STS_HALTED, 0, 50))
	{
		lr_log("g3: FAIL controller did not start, sts %08x\n", lr_rd(op, USBSTS));
		goto release;
	}

	lr_wr(op, CONFIGFLAG, 1);
	lr_mdelay(5);

	if (hcs & 0x10)
	{
		for (i = 0; i < nports; i++)
		{
			u32 v = lr_rd(op, PORTSC(i));

			if (!(v & P_POWER))
				lr_wr(op, PORTSC(i), (v & ~P_RWC) | P_POWER);
		}
	}

	lr_mdelay(120);		/* power-good + connect debounce */

	for (i = 0; i < nports; i++)
	{
		u32 v = lr_rd(op, PORTSC(i));

		lr_log("g3: p%d %08x\n", i + 1, v);

		if (!(v & P_CCS))
		{
			port_to_companion(i);
			continue;
		}

		if (((v & P_LINE) >> 10) == 1)
		{
			lr_log("g3: p%d low-speed device, to companion\n", i + 1);
			port_to_companion(i);
			continue;
		}

		if (found)
		{
			/* one device is enough for this gate; leave any other to the companion */
			port_to_companion(i);
			continue;
		}

		if (!port_reset(i))
		{
			lr_log("g3: p%d full-speed device, to companion\n", i + 1);
			port_to_companion(i);
			continue;
		}

		found = 1;
		lr_mdelay(20);		/* reset recovery */
		lr_log("g3: p%d HIGH-SPEED device enabled\n", i + 1);
		lr_log_flush();

		if (get_device_descriptor(page, (u32)bus) == 0)
		{
			pass = 1;
			lr_log("g3: PASS port %d\n", i + 1);
		}
		else
			lr_log("g3: FAIL transfer on port %d\n", i + 1);
	}

	if (!found)
		lr_log("g3: no high-speed device on any of the %d ports of controller 0\n", nports);

release:
	halt_and_release();
	lr_mdelay(20);
	dma_put(bus_id, dev_id, bus, len, m);
	lr_log("g3: done pass %d\n", pass);
}
