/* UDP broadcast mirror of the log through the lv1 gelic device.
 *
 * Derived from Cobra debug/src/debug.c (marcan, GPLv2), which no longer compiles for PS2EMU in
 * the 8.5 tree (it references lv2's syscall table). Differences that matter:
 *   - init is lazy and repeatable: if the emulator has not opened the gelic device yet,
 *     the DMA map fails and we simply try again later;
 *   - the transmit wait has a timeout (no cable must not hang the emulator);
 *   - a real source address and a full IP header checksum, so an ordinary UDP socket on the
 *     bench PC receives the datagrams.
 * We never open or close the device ourselves: a second lv1_open_device from the emulator
 * would fail and it asserts on that.
 */
#include <ps2emu/ps2emu.h>
#include <ps2emu/libc.h>
#include <lv1/lv1.h>
#include <lv1/gelic.h>
#include <lv1/device.h>
#include "lr.h"

#define MAX_MSG		1000
#define TX_TIMEOUT_US	10000
#define MAX_TRIES	6
#define MAX_TX_FAIL	3

struct lr_netblk
{
	volatile struct gelic_descr descr;
	u8 pkt[1200];
} __attribute__((aligned(32)));

static struct lr_netblk blk;
static int net_up, net_tries, tx_fail;
static int bus_id, dev_id;
static u64 bus_addr;
static int hdr_len;
static u8 *ip, *udp, *payload;

int lr_net_state(void)
{
	return net_up;
}

static void put16(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 8);
	p[1] = (u8)v;
}

static void put32(u8 *p, u32 v)
{
	put16(p, v >> 16);
	put16(p + 2, v);
}

void lr_net_try(void)
{
	u64 mac, vlan, v2;
	s64 r;
	u8 *p = blk.pkt;

	if (net_up || net_tries >= MAX_TRIES)
		return;
	net_tries++;

	if (find_device_by_type(DEV_TYPE_GELIC, 0, &bus_id, &dev_id, NULL))
	{
		lr_log("net: no gelic device in the repository\n");
		net_tries = MAX_TRIES;
		return;
	}

	r = map_dma_mem(bus_id, dev_id, &blk, sizeof(blk), &bus_addr);
	if (r)
	{
		lr_log("net: try %d dma map failed %ld (bus %d dev %d)\n", net_tries, (long)r, bus_id, dev_id);
		return;
	}

	memset(&blk, 0, sizeof(blk));
	blk.descr.buf_addr = (u32)(bus_addr + offsetof(struct lr_netblk, pkt));

	r = lv1_net_control(bus_id, dev_id, GELIC_LV1_GET_MAC_ADDRESS, 0, 0, 0, &mac, &v2);
	if (r)
	{
		lr_log("net: get mac failed %ld\n", (long)r);
		unmap_dma_mem(bus_id, dev_id, bus_addr, sizeof(blk));
		return;
	}

	memset(p, 0xff, 6);			/* dest: broadcast */
	mac <<= 16;
	memcpy(p + 6, &mac, 6);
	p += 12;

	if (lv1_net_control(bus_id, dev_id, GELIC_LV1_GET_VLAN_ID, GELIC_LV1_VLAN_TX_ETHERNET_0, 0, 0, &vlan, &v2) == 0)
	{
		put16(p, 0x8100);
		put16(p + 2, (u32)vlan);
		p += 4;
	}
	put16(p, 0x0800);
	p += 2;

	ip = p;
	ip[0] = 0x45;
	ip[8] = 16;				/* ttl */
	ip[9] = 0x11;				/* udp */
	put32(ip + 12, LR_SRC_IP);
	put32(ip + 16, 0xffffffff);
	udp = ip + 20;
	put16(udp, LR_UDP_PORT);
	put16(udp + 2, LR_UDP_PORT);
	payload = udp + 8;
	hdr_len = (int)(payload - blk.pkt);

	net_up = 1;
	lr_log("net: up on try %d, bus %d dev %d, dma %lx\n", net_tries, bus_id, dev_id, (unsigned long)bus_addr);
}

/* give up for good: no more init attempts either */
static void net_abandon(void)
{
	net_up = 0;
	net_tries = MAX_TRIES;
}

void lr_net_send(const char *msg, int len)
{
	u32 sum = 0;
	u64 end;
	int i;

	if (!net_up || len <= 0)
		return;
	if (len > MAX_MSG)
		len = MAX_MSG;

	memcpy(payload, (void *)msg, len);

	put16(ip + 2, 20 + 8 + len);
	put16(ip + 10, 0);
	for (i = 0; i < 20; i += 2)
		sum += ((u32)ip[i] << 8) | ip[i + 1];
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	put16(ip + 10, ~sum);

	put16(udp + 4, 8 + len);
	put16(udp + 6, 0);

	blk.descr.buf_size = hdr_len + len;
	blk.descr.next_descr_addr = 0;
	blk.descr.result_size = 0;
	blk.descr.data_status = 0;
	blk.descr.dmac_cmd_status = GELIC_DESCR_DMA_CMD_NO_CHKSUM | GELIC_DESCR_TX_DMA_FRAME_TAIL;

	if (lv1_net_start_tx_dma(bus_id, dev_id, bus_addr, 0))
	{
		if (++tx_fail >= MAX_TX_FAIL)
			net_abandon();
		return;
	}

	end = lr_tb() + (TX_TIMEOUT_US * LR_TB_PER_MS) / 1000;
	while ((blk.descr.dmac_cmd_status & GELIC_DESCR_DMA_STAT_MASK) == GELIC_DESCR_DMA_CARDOWNED)
	{
		if ((s64)(lr_tb() - end) > 0)
		{
			lv1_net_stop_tx_dma(bus_id, dev_id, 0);
			if (++tx_fail >= MAX_TX_FAIL)
				net_abandon();	/* no link: the RAM/file log carries on */
			return;
		}
	}
}
