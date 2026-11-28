/* LR-G3b/G4: enumerate the one device on our root port and drive it as a bulk-only mass
 * storage device. Everything is polled through lr_hc.c; nothing here touches a register.
 *
 * Rules this file keeps:
 *   - Every IN transfer asks for exactly the length the device will send. A short packet
 *     raises USBINT on the controller we share with the emulator, so none is provoked.
 *   - The device address is LR_USB_ADDR (127): both root ports hear every token, and the
 *     emulator's own device was seen at address 62 on the mule (2026-11-21). lr_hc.c refuses
 *     to attach if a queue head of the emulator's already uses 127.
 */
#include <ps2emu/ps2emu.h>
#include <ps2emu/libc.h>
#include "lr.h"

lr_usbdev lr_dev;
u32 lr_blk_count;
int lr_lun = -1;

static u32 msc_tag = 0x50520000;

/* ------------------------------------------------------------------ control */

static int ctrl(u32 rt, u32 req, u32 val, u32 idx, void *data, u32 len)
{
	u8 s[8];

	s[0] = (u8)rt;
	s[1] = (u8)req;
	s[2] = (u8)val;
	s[3] = (u8)(val >> 8);
	s[4] = (u8)idx;
	s[5] = (u8)(idx >> 8);
	s[6] = (u8)len;
	s[7] = (u8)(len >> 8);

	return lr_hc_ctrl(s, data, (int)len);
}

int lr_usb_clear_halt(int q)
{
	u32 ep = (q == LR_Q_IN) ? lr_dev.ep_in : lr_dev.ep_out;
	int r = ctrl(0x02, 1, 0, ep, NULL, 0);

	lr_hc_toggle0(q);
	return r;
}

/* The first request after a reset, and the question bench run R2 raised on 2026-11-21: the
 * reader STALLed GET_DESCRIPTOR sent with the bytes in memory order. This bridge presents its
 * registers and schedule structures big-endian; if it does that by reversing every 32-bit word
 * it moves, DATA is reversed too and the request arrived garbled. So both byte orders are
 * tried, with a port reset in between, and the one that returns a real device descriptor is
 * kept for the session. Every try is logged. */
static int first_descriptor(u8 *d)
{
	static u8 known;
	int tries = known ? 2 : 6, t, r = LR_E_STATE;

	for (t = 0; t < tries; t++)
	{
		if (!known)
			lr_swap = (u8)!(t & 1);	/* reversed first: that is what the mule's bridge does */
		if (t)
		{
			if (t >= 2)
				lr_mdelay(100);
			r = lr_hc_port_reset();
			if (r)
				return r;
		}

		memset(d, 0, 18);
		lr_hc_pipe(LR_Q_CTRL, 0, 0, 64);	/* high speed: endpoint 0 is always 64 bytes */
		r = ctrl(0x80, 6, 0x0100, 0, d, 18);
		lr_log("usb: descriptor try %d, word-swap %d -> %d (%02x %02x %02x %02x)\n", t, lr_swap, r, d[0], d[1], d[2], d[3]);
		if (r == 0 && d[0] == 18 && d[1] == 1)
		{
			known = 1;
			return 0;
		}
		if (r == 0)
			r = LR_E_STATE;
	}

	return r;
}

#if defined(PS2HWEMU)
/* A01, the first CECHA log (2026-11-27): SET_ADDRESS 127 went through, and the next request at
 * 127 ended in BABBLE with ANOTHER device's descriptor in the buffer (12 01 00 02 e0 01 01 40:
 * class e0/01/01, Bluetooth), not the reader's (class 00). So something else answers at that
 * address on this bus. Which device, and why it answered a config request with a device
 * descriptor, is not known. An address is now used only if (1) nothing answers there while the
 * reader is still at 0, and (2) after SET_ADDRESS the reader's own descriptor, all 18 bytes,
 * comes back from it. 127 first, then down to 124. Every refusal is logged. */
static int set_addr(const u8 *d)
{
	u8 e[18];
	u32 a;
	int r;

	for (a = LR_USB_ADDR; a > LR_USB_ADDR - 4; a--)
	{
		memset(e, 0, 8);
		lr_hc_pipe(LR_Q_CTRL, a, 0, 64);
		r = ctrl(0x80, 6, 0x0100, 0, e, 8);
		if (r != LR_E_XACT || (lr_hc_tok & 0x78) != 0x48)
		{
			lr_log("usb: addr %d answers: %d tok %08x, %02x %02x %02x %02x %02x %02x %02x %02x\n", a, r, lr_hc_tok, e[0], e[1],
				e[2], e[3], e[4], e[5], e[6], e[7]);
			continue;
		}

		lr_hc_pipe(LR_Q_CTRL, 0, 0, 64);
		r = ctrl(0x00, 5, a, 0, NULL, 0);
		if (r)
			return r;
		lr_mdelay(5);		/* set-address recovery */
		lr_hc_pipe(LR_Q_CTRL, a, 0, 64);
		r = ctrl(0x80, 6, 0x0100, 0, e, 18);
		if (r == 0 && !memcmp(e, d, 18))
		{
			lr_dev.addr = (u8)a;
			return 0;
		}
		lr_log("usb: addr %d is not the reader after SET_ADDRESS: %d, %02x %02x %02x %02x %02x %02x %02x %02x\n", a, r, e[0],
			e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
		r = lr_hc_port_reset();		/* the reader back at 0 for the next address */
		if (r)
			return r;
	}

	return LR_E_STATE;
}
#endif

int lr_usb_enumerate(void)
{
	u8 d[18];
	u8 c[256];
	u8 b = 0;
	int r, total, i, in_msc = 0;

	memset(&lr_dev, 0, sizeof(lr_dev));
	lr_blk_count = 0;

	r = first_descriptor(d);
	if (r)
	{
		lr_log("usb: device descriptor failed %d\n", r);
		return r;
	}

	lr_dev.vid = (u16)(d[8] | (d[9] << 8));
	lr_dev.pid = (u16)(d[10] | (d[11] << 8));
	lr_log("usb: VID %04x PID %04x bcdUSB %02x%02x class %02x mps0 %d\n", lr_dev.vid, lr_dev.pid, d[3], d[2], d[4], d[7]);

#if defined(PS2HWEMU)
	r = set_addr(d);
	if (r)
	{
		lr_log("usb: SET_ADDRESS failed %d\n", r);
		return r;
	}
#define DEV_ADDR	lr_dev.addr
#else
	r = ctrl(0x00, 5, LR_USB_ADDR, 0, NULL, 0);
	if (r)
	{
		lr_log("usb: SET_ADDRESS failed %d\n", r);
		return r;
	}
	lr_mdelay(5);		/* set-address recovery */
	lr_hc_pipe(LR_Q_CTRL, LR_USB_ADDR, 0, 64);
	lr_dev.addr = LR_USB_ADDR;
#define DEV_ADDR	LR_USB_ADDR
#endif

	r = ctrl(0x80, 6, 0x0200, 0, c, 9);
	if (r || c[1] != 2)
	{
		lr_log("usb: config header failed %d type %02x\n", r, c[1]);
		return r ? r : LR_E_STATE;
	}

	total = c[2] | (c[3] << 8);
	if (total > 255)
		total = 255;
	if (total < 9 + 9 + 7 + 7)
	{
		lr_log("usb: config descriptor only %d bytes\n", total);
		return LR_E_STATE;
	}

	r = ctrl(0x80, 6, 0x0200, 0, c, (u32)total);
	if (r)
	{
		lr_log("usb: config descriptor failed %d\n", r);
		return r;
	}
	lr_dev.cfg = c[5];

	for (i = 0; i + 2 <= total && c[i] >= 2 && i + c[i] <= total; i += c[i])
	{
		if (c[i + 1] == 4 && c[i] >= 9)
		{
			/* first bulk-only mass storage interface, alternate setting 0 */
			in_msc = (c[i + 5] == 8 && c[i + 7] == 0x50 && c[i + 3] == 0 && !(lr_dev.ep_in && lr_dev.ep_out));
			if (in_msc)
			{
				lr_dev.iface = c[i + 2];
				lr_dev.ep_in = lr_dev.ep_out = 0;
				lr_dev.subclass = c[i + 6];
				lr_log("usb: msc interface %d subclass %02x\n", c[i + 2], c[i + 6]);
			}
		}
		else if (c[i + 1] == 5 && c[i] >= 7 && in_msc && (c[i + 3] & 3) == 2)
		{
			u16 mps = (u16)((c[i + 4] | (c[i + 5] << 8)) & 0x7ff);

			if ((c[i + 2] & 0x80) && !lr_dev.ep_in)
			{
				lr_dev.ep_in = c[i + 2];
				lr_dev.mps_in = mps;
			}
			else if (!(c[i + 2] & 0x80) && !lr_dev.ep_out)
			{
				lr_dev.ep_out = c[i + 2];
				lr_dev.mps_out = mps;
			}
		}
	}

	if (!lr_dev.ep_in || !lr_dev.ep_out || !lr_dev.mps_in || !lr_dev.mps_out)
	{
		lr_log("usb: no bulk-only mass storage interface (in %02x out %02x)\n", lr_dev.ep_in, lr_dev.ep_out);
		return LR_E_STATE;
	}

	r = ctrl(0x00, 9, lr_dev.cfg, 0, NULL, 0);
	if (r)
	{
		lr_log("usb: SET_CONFIGURATION failed %d\n", r);
		return r;
	}

	/* GET MAX LUN. A single-LUN device may stall it; that means 0. */
	r = ctrl(0xA1, 0xFE, 0, lr_dev.iface, &b, 1);
	lr_dev.maxlun = (r == 0 && b < 8) ? b : 0;

	lr_hc_pipe(LR_Q_OUT, DEV_ADDR, lr_dev.ep_out & 15, lr_dev.mps_out);
	lr_hc_pipe(LR_Q_IN, DEV_ADDR, lr_dev.ep_in & 15, lr_dev.mps_in);

	lr_log("usb: addr %d cfg %d ep in %02x/%d out %02x/%d maxlun %d (%d)\n", lr_dev.addr, lr_dev.cfg, lr_dev.ep_in,
		lr_dev.mps_in, lr_dev.ep_out, lr_dev.mps_out, lr_dev.maxlun, r);

	return 0;
}

/* --------------------------------------------------------- bulk-only transport */

static void put_le32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

/* One command with a data-in stage of exactly len bytes at lr_dma + off (or none).
 * Returns 0 = good status, 1 = the device reported the command failed, < 0 = transport. */
int lr_msc_cmd(u32 lun, const u8 *cb, int cblen, u32 off, u32 len)
{
	u8 *cbw = lr_dma + LR_OFF_CBW, *csw = lr_dma + LR_OFF_CSW;
	int r, i, tries, stalled = 0;

	memset(cbw, 0, 31);
	cbw[0] = 0x55;
	cbw[1] = 0x53;
	cbw[2] = 0x42;
	cbw[3] = 0x43;
	put_le32(cbw + 4, ++msc_tag);
	put_le32(cbw + 8, len);
	cbw[12] = 0x80;
	cbw[13] = (u8)lun;
	cbw[14] = (u8)cblen;
	for (i = 0; i < cblen; i++)
		cbw[15 + i] = cb[i];
	if (lr_dev.subclass == 0x02 || lr_dev.subclass == 0x05)
	{
		/* ATAPI-style command sets (SFF-8020i / SFF-8070i; the PS3's own reader reports 05):
		 * every command is 12 bytes, zero padded, with the LUN repeated in byte 1 */
		cbw[14] = 12;
		cbw[16] = (u8)((cbw[16] & 0x1f) | (lun << 5));
	}

	r = lr_hc_bulk(LR_Q_OUT, LR_OFF_CBW, 31, 2000);
	if (r)
		return r;

	if (len)
	{
		r = lr_hc_bulk(LR_Q_IN, off, len, 5000);
		if (r == LR_E_STALL || r == LR_E_SHORT)
		{
			/* the device had less (or nothing) to send: its status follows */
			if (r == LR_E_STALL && lr_usb_clear_halt(LR_Q_IN))
				return LR_E_XACT;
			stalled = 1;
		}
		else if (r)
			return r;
	}

	memset(csw, 0, 13);
	for (tries = 0;; tries++)
	{
		r = lr_hc_bulk(LR_Q_IN, LR_OFF_CSW, 13, 5000);
		if (r == LR_E_STALL && tries == 0)
		{
			if (lr_usb_clear_halt(LR_Q_IN))
				return LR_E_XACT;
			continue;
		}
		if (r)
			return r;
		break;
	}

	if (csw[0] != 0x55 || csw[1] != 0x53 || csw[2] != 0x42 || csw[3] != 0x53 || memcmp(csw + 4, cbw + 4, 4) != 0)
	{
		lr_log("msc: bad status block %02x%02x%02x%02x tag %02x%02x%02x%02x\n", csw[0], csw[1], csw[2], csw[3], csw[4],
			csw[5], csw[6], csw[7]);
		return LR_E_STATE;
	}

	if (csw[12] == 0)
		return stalled ? 1 : 0;
	if (csw[12] == 1)
		return 1;

	return LR_E_STATE;	/* phase error: the caller re-enumerates */
}

static void cb_clear(u8 *cb, u8 op)
{
	int i;

	for (i = 0; i < 10; i++)
		cb[i] = 0;
	cb[0] = op;
}

/* REQUEST SENSE: returns (key << 16) | (asc << 8) | ascq, or a negative transport error. */
static int scsi_sense(u32 lun)
{
	u8 cb[10], *d = lr_dma + LR_OFF_META;
	int r;

	cb_clear(cb, 0x03);
	cb[4] = 18;
	r = lr_msc_cmd(lun, cb, 6, LR_OFF_META, 18);
	if (r < 0)
		return r;
	if (r)
		return 0x7fffff;

	return ((d[2] & 15) << 16) | (d[12] << 8) | d[13];
}

int lr_blk_read(u32 lba, u32 n, u32 off)
{
	u8 cb[10];
	int r;

	if (lr_lun < 0 || !n || n > 0xffff || lba >= lr_blk_count || n > lr_blk_count - lba)
		return LR_E_STATE;

	cb_clear(cb, 0x28);
	cb[2] = (u8)(lba >> 24);
	cb[3] = (u8)(lba >> 16);
	cb[4] = (u8)(lba >> 8);
	cb[5] = (u8)lba;
	cb[7] = (u8)(n >> 8);
	cb[8] = (u8)n;

	r = lr_msc_cmd((u32)lr_lun, cb, 10, off, n * 512);
	if (r > 0)
	{
		lr_log("msc: READ lba %x n %d failed, sense %06x\n", lba, (int)n, scsi_sense((u32)lr_lun));
		return LR_E_STATE;
	}

	return r;
}

static void inquiry(u32 lun)
{
	u8 cb[10], *d = lr_dma + LR_OFF_META;
	char vend[9], prod[17];
	int r, i;

	cb_clear(cb, 0x12);
	cb[4] = 36;
	r = lr_msc_cmd(lun, cb, 6, LR_OFF_META, 36);
	if (r)
	{
		lr_log("msc: lun %d INQUIRY %d\n", (int)lun, r);
		return;
	}

	for (i = 0; i < 8; i++)
		vend[i] = (d[8 + i] >= 32 && d[8 + i] < 127) ? (char)d[8 + i] : '?';
	vend[8] = 0;
	for (i = 0; i < 16; i++)
		prod[i] = (d[16 + i] >= 32 && d[16 + i] < 127) ? (char)d[16 + i] : '?';
	prod[16] = 0;
	lr_log("msc: lun %d type %02x rmb %02x [%s] [%s]\n", (int)lun, d[0], d[1], vend, prod);
}

/* Find a LUN with a medium in it. want < 0 takes the first one that is ready.
 * A reader that was suspended until a moment ago needs time to power its card, so "no
 * medium" is believed only after "rounds" x 100 ms. */
int lr_msc_select(int want, int rounds)
{
	u8 cb[10], *d = lr_dma + LR_OFF_META;
	int round, lun, r, last_sense[8];

	lr_lun = -1;
	lr_blk_count = 0;
	for (lun = 0; lun < 8; lun++)
		last_sense[lun] = -1;

	for (round = 0; round < rounds; round++)
	{
		for (lun = 0; lun <= lr_dev.maxlun; lun++)
		{
			if (want >= 0 && lun != want)
				continue;

			if (round == 0)
				inquiry((u32)lun);

			cb_clear(cb, 0x00);
			r = lr_msc_cmd((u32)lun, cb, 6, 0, 0);
			if (r < 0)
				return r;

			if (r > 0)
			{
				int s = scsi_sense((u32)lun);

				if (s < 0)
					return s;
				if (s != last_sense[lun])
				{
					last_sense[lun] = s;
					lr_log("msc: lun %d not ready, sense %06x (round %d)\n", lun, s, round);
				}
				continue;
			}

			cb_clear(cb, 0x25);
			r = lr_msc_cmd((u32)lun, cb, 10, LR_OFF_META, 8);
			if (r < 0)
				return r;
			if (r > 0)
				continue;

			{
				u32 last = ((u32)d[0] << 24) | ((u32)d[1] << 16) | ((u32)d[2] << 8) | d[3];
				u32 bs = ((u32)d[4] << 24) | ((u32)d[5] << 16) | ((u32)d[6] << 8) | d[7];

				lr_log("msc: lun %d ready, %u blocks of %u (round %d)\n", lun, last + 1, bs, round);
				if (bs != 512 || last == 0xffffffff || last < 64)
				{
					lr_log("msc: lun %d unsupported geometry\n", lun);
					continue;
				}

				lr_lun = lun;
				lr_blk_count = last + 1;
				return 0;
			}
		}

		lr_mdelay(100);
	}

	lr_log("msc: no LUN with a readable medium\n");
	return LR_E_NOMEDIUM;
}

#if defined(LR_OPL)
/* OPL (G1U): READ(10) of n blocks at lba into lr_dma + off, the data and status stages left running
 * (lr_hc_in2_start). lr_blk_read_poll() ends it: 1 = still running (wait 0), 0 = read, > 0 = the
 * device failed it, < 0 = transport (LR_E_*). The caller recovers as for lr_blk_read. */
static u32 pend_tag;

int lr_blk_read_start(u32 lba, u32 n, u32 off)
{
	u8 *cbw = lr_dma + LR_OFF_CBW;
	int r;

	if (lr_lun < 0 || !n || n > 0xffff || lba >= lr_blk_count || n > lr_blk_count - lba)
		return LR_E_STATE;

	memset(cbw, 0, 31);
	cbw[0] = 0x55;
	cbw[1] = 0x53;
	cbw[2] = 0x42;
	cbw[3] = 0x43;
	pend_tag = ++msc_tag;
	put_le32(cbw + 4, pend_tag);
	put_le32(cbw + 8, n * 512);
	cbw[12] = 0x80;
	cbw[13] = (u8)lr_lun;
	cbw[14] = 10;
	cbw[15] = 0x28;
	cbw[17] = (u8)(lba >> 24);
	cbw[18] = (u8)(lba >> 16);
	cbw[19] = (u8)(lba >> 8);
	cbw[20] = (u8)lba;
	cbw[22] = (u8)(n >> 8);
	cbw[23] = (u8)n;
	if (lr_dev.subclass == 0x02 || lr_dev.subclass == 0x05)
	{
		/* ATAPI-style command sets, as lr_msc_cmd: a 12-byte packet, the LUN in byte 1's top bits. The PS3's
		 * own reader reports 05 (G1U on the E01, 11-28: refusing it here cost a card reset per prefetch) */
		cbw[14] = 12;
		cbw[16] = (u8)(lr_lun << 5);
	}

	r = lr_hc_bulk(LR_Q_OUT, LR_OFF_CBW, 31, 2000);
	if (r)
		return r;
	memset(lr_dma + LR_OFF_CSW, 0, 16);
	return lr_hc_in2_start(off, n * 512, LR_OFF_CSW, 13);
}

int lr_blk_read_poll(int wait)
{
	u8 *csw = lr_dma + LR_OFF_CSW;
	int r = lr_hc_in2_poll(5000, wait);

	if (r)
		return r;
	if (csw[0] != 0x55 || csw[1] != 0x53 || csw[2] != 0x42 || csw[3] != 0x53 || csw[4] != (u8)pend_tag ||
		csw[5] != (u8)(pend_tag >> 8) || csw[6] != (u8)(pend_tag >> 16) || csw[7] != (u8)(pend_tag >> 24))
	{
		lr_log("msc: bad status block (in flight) %02x%02x%02x%02x tag %02x%02x%02x%02x\n", csw[0], csw[1], csw[2], csw[3],
			csw[4], csw[5], csw[6], csw[7]);
		return LR_E_STATE;
	}
	if (csw[12] == 0)
		return 0;
	if (csw[12] == 1)
		return 1;
	return LR_E_STATE;
}
#endif
