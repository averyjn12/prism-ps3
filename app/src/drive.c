/* v0.38: every command Prism sends the disc drive to copy a PS1 / PS2 disc. Nothing here is
 * invented: each call is copied from code that does this job on real consoles, named beside it.
 *
 *   [C]  the Cobra library ("cobralib", from Cobra USB Manager), as shipped in ManaGunZ
 *        (github.com/Zarh/ManaGunZ @ 4a11fa4b99ce6166ae633a9e8c08c846967627a1,
 *        MGZ/lib/cobra/source/cobra.c and storage.h) and in webMAN-MOD (@ 6bd44252, cobra/cobra.c).
 *   [M]  multiMAN 04.85.01 (mmCM), the copy installed on Bananafuzzbuddy's console as BLES80608: its
 *        RELOAD.SELF decrypted on the PC (sha256 of the ELF a72f0d08...9cce3f), addresses below.
 *        It contains [C] compiled in, and its "Create ISO" (function at 0xa514c) calls it.
 *   [I]  IRISMAN (github.com/aldostools/IRISMAN @ 91ac4e820e76fb9c1bbf9ec0cdd7592deb35f70d),
 *        source/psx.c and include/storage.h.
 *
 * Why v0.32 froze the console (prism_rip.log 11-23 ended at "drive open rc 0"): its READ TOC asked
 * for 804 bytes at once with a 10-byte packet length. [C] and [I] both ask for the 4-byte header
 * first and then for exactly the length the header names, with a 12-byte packet: a PIO transfer is
 * told how many bytes to wait for, and a disc with one track only ever sends 20. That is the likely
 * cause, not a proven one - so each command below still asks before its first run on a console
 * (rip.c drive_step) and is logged before it is sent.
 *
 * No LV1 / LV2 patch is involved: [I] patches the kernel only when it runs WITHOUT Cobra/Mamba
 * (source/main.c L2547-2578: "if(!use_mamba) ... lv2_patch_storage()", else it is set to NULL), and
 * [M] in Cobra mode makes these calls as they are. */
#include <string.h>
#include <unistd.h>
#include <ppu-lv2.h>
#include "drive.h"

static void (*lg)(const char *fmt, ...);

void drv_set_log(void (*fn)(const char *fmt, ...))
{
	lg = fn;
}

#define LOG(...)	do { if (lg) lg(__VA_ARGS__); } while (0)

/* [C] storage.h L179-183: sys_ss_disc_auth = lv2syscall2(864, func, param) */
static s32 sc864(u64 func, u64 param)
{
	lv2syscall2(864, func, param);
	return_to_user_prog(s32);
}

/* [C] storage.h L95-99: sys_storage_send_device_command = lv2syscall6(604, handle, command, indata,
 * inlen, outdata, outlen). Six arguments. ([I] include/storage.h L134-146 has the same six.) */
static s32 sc604(u32 fd, u32 cmd, void *in, u32 inlen, void *out, u32 outlen)
{
	lv2syscall6(604, fd, cmd, (u64)(unsigned long)in, inlen, (u64)(unsigned long)out, outlen);
	return_to_user_prog(s32);
}

/* [C] cobra.c L772-798 cobra_disc_auth(): for a disc that is not PS3_BD / PS3_DVD it is
 * sys_ss_disc_auth(0x5004, 0x29), result ignored. [M] 0x13a630-0x13a63c: li r3,0x5004; li r4,0x29;
 * li r11,864; sc - called at 0xa5270, the first thing "Create ISO" does in Cobra mode.
 * psdevwiki Storage_Manager: 0x5004 / 0x29 = "Reset BD Drive + cleans key".
 * [I] source/psx.c L2514-2524 repeats it while it answers 0x8001000A (busy); bounded here. */
s32 drv_auth_reset(void)
{
	s32 r = 0;
	int n;

	LOG("drive: next is syscall 864 (0x5004, 0x29), the drive reset multiMAN and the Cobra library use");
	for (n = 0; n < 200; n++)
	{
		r = sc864(0x5004, 0x29);
		if ((u32)r != DRV_EBUSY)
			break;
		usleep(10000);
	}
	LOG("  drive reset: result %x after %d busy answer(s)", (u32)r, n);
	return r;
}

/* The 56-byte command block: 32 bytes of packet, then [C] StorageCmdScsiData (storage.h L69-76):
 * inlen, unk1, outlen, unk2, unk3 - [C] sets inlen 12 and the three unknowns 1 for every read. ([I]
 * names the same five words pktlen, blocks, block_size, proto, in_out: 1 block, PIO in, read.) */
typedef struct
{
	u8 pkt[32];
	u32 inlen, unk1, outlen, unk2, unk3, pad;
} __attribute__((packed)) cmd56;

/* blocks of block_size bytes come back; proto 1 = PIO in ([C], every read), 3 = DMA ([I]'s reads) */
static s32 send_as(u32 fd, cmd56 *c, void *out, u32 blocks, u32 block_size, u32 proto)
{
	s32 r = 0;
	int n;

	c->inlen = 12;
	c->unk1 = blocks;
	c->outlen = block_size;
	c->unk2 = proto;
	c->unk3 = 1;		/* device -> memory */
	/* [M] 0x2ddb8-0x2ddf8 and 0x62318-0x62348: its own 604 calls are repeated, 10 ms apart, while
	 * the answer is 0x8001000A, at most 100 times */
	for (n = 0; n < 100; n++)
	{
		r = sc604(fd, 1 /* STORAGE_COMMAND_NATIVE, [C] storage.h L51 */, c, sizeof *c, out, blocks * block_size);
		if ((u32)r != DRV_EBUSY)
			break;
		usleep(10000);
	}
	return r;
}

/* [C]'s form: one block of outlen bytes, PIO (the five words 12, 1, outlen, 1, 1) */
static s32 send(u32 fd, cmd56 *c, void *out, u32 outlen)
{
	return send_as(fd, c, out, 1, outlen, 1);
}

/* [C] cobra.c L973-1003 cobra_get_cd_td(): opcode 0x43 (READ TOC/PMA/ATIP), every other packet byte
 * 0 but the allocation length (bytes 7-8); first with the length of the 4-byte header, then with
 * toc_length + 2. [M] 0x1395e4-0x1395fc (outlen 4) and 0x139670-0x139688 (outlen toc_length + 2). */
static s32 toc_cmd(u32 fd, u8 *out, u32 alloc)
{
	cmd56 c;

	memset(&c, 0, sizeof c);
	c.pkt[0] = 0x43;
	c.pkt[7] = (u8)(alloc >> 8);
	c.pkt[8] = (u8)alloc;
	return send(fd, &c, out, alloc);
}

s32 drv_toc(u32 fd, u8 *out, int *len)
{
	static u8 hdr[64] __attribute__((aligned(64)));
	u32 want;
	s32 r;

	*len = 0;
	memset(hdr, 0, sizeof hdr);
	LOG("drive: next is READ TOC (syscall 604, 56-byte block, opcode 0x43) for the 4-byte header");
	r = toc_cmd(fd, hdr, 4);
	LOG("  TOC header: result %x, bytes %02x %02x %02x %02x", (u32)r, hdr[0], hdr[1], hdr[2], hdr[3]);
	if (r)
		return r;
	want = ((u32)hdr[0] << 8 | hdr[1]) + 2;
	if (want < 20 || want > DRV_TOC_MAX)
	{
		LOG("  TOC length %u is not a track list's: nothing more asked", (unsigned)want);
		return -1;
	}
	LOG("drive: next is READ TOC for all %u bytes", (unsigned)want);
	r = toc_cmd(fd, out, want);
	LOG("  TOC: result %x", (u32)r);
	if (r)
		return r;
	*len = (int)want;
	return 0;
}

/* [C] cobra.c L1089-1152 cobra_cd_read(): opcode 0xBE (READ CD), the sector big-endian in bytes
 * 2-5, a count of one sector in bytes 6-8, byte 9 = 0xF8 for a data sector (sync, header, user
 * data, EDC/ECC: 2352 bytes) or 0x10 inside an audio track (user data: 2352 bytes); one sector per
 * command, outlen 2352. [M] 0x13a308-0x13a3c4 is this code byte for byte. A data sector read with
 * the audio flag would answer with fewer bytes than asked for: the caller only sets audio inside an
 * audio track of a disc whose first track alone is data (ripcore.c rc_toc_parse refuses the rest). */
s32 drv_read_cd(u32 fd, u32 lba, int audio, u8 *buf, int say)
{
	cmd56 c;
	s32 r;

	memset(&c, 0, sizeof c);
	c.pkt[0] = 0xBE;
	c.pkt[2] = (u8)(lba >> 24);
	c.pkt[3] = (u8)(lba >> 16);
	c.pkt[4] = (u8)(lba >> 8);
	c.pkt[5] = (u8)lba;
	c.pkt[8] = 1;
	c.pkt[9] = audio ? 0x10 : 0xF8;
	if (say)
		LOG("drive: next is READ CD (syscall 604, opcode 0xBE, flags %02x) for sector %u", c.pkt[9], (unsigned)lba);
	r = send(fd, &c, buf, 2352);
	if (say)
		LOG("  READ CD %u: result %x, first bytes %02x %02x %02x %02x .. mode byte %02x", (unsigned)lba, (u32)r, buf[0], buf[1],
			buf[2], buf[3], buf[15]);
	return r;
}

/* v0.44, the fast read. [I] source/psx.c L2730-2760 read_raw_sector_bdvd(sector, buffer, 0x940, n),
 * as copy_PSX_game_from_CD (L3022-3087) calls it for a whole PS1 disc, data and music alike: opcode
 * 0xBE, the sector in bytes 2-5, the count in bytes 6-8, byte 9 = 0xF8, byte 10 = 2 (the 16 bytes of
 * formatted Q sub-channel after each sector, so 0x940 = 2368 bytes a sector); the five words are
 * 12, n, 0x940, 3 (ATAPI_DMA_PROTO, include/storage.h L95), 1; the buffer n * 0x940 bytes. [I] asks
 * for up to 1024 sectors at once (and any smaller count for a disc's last block), and for 1 when it
 * tries a block again sector by sector (L3066). Prism asks for at most DRV_FAST_MAX. */
s32 drv_read_cd_n(u32 fd, u32 lba, u32 n, u8 *buf, int say)
{
	cmd56 c;
	s32 r;

	if (!n || n > DRV_FAST_MAX)
		return -1;
	memset(&c, 0, sizeof c);
	c.pkt[0] = 0xBE;
	c.pkt[2] = (u8)(lba >> 24);
	c.pkt[3] = (u8)(lba >> 16);
	c.pkt[4] = (u8)(lba >> 8);
	c.pkt[5] = (u8)lba;
	c.pkt[6] = (u8)(n >> 16);
	c.pkt[7] = (u8)(n >> 8);
	c.pkt[8] = (u8)n;
	c.pkt[9] = 0xF8;
	c.pkt[10] = 2;
	if (say)
		LOG("drive: next is READ CD the fast way (syscall 604, opcode 0xBE, flags f8, sub-channel 2, DMA) for %u sector(s) at %u",
			(unsigned)n, (unsigned)lba);
	r = send_as(fd, &c, buf, n, DRV_RAWQ, 3);
	if (say)
		LOG("  fast READ CD %u + %u: result %x, first bytes %02x %02x %02x %02x .. mode byte %02x, sub-channel %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			(unsigned)lba, (unsigned)n, (u32)r, buf[0], buf[1], buf[2], buf[3], buf[15], buf[2352], buf[2353], buf[2354], buf[2355],
			buf[2356], buf[2357], buf[2358], buf[2359], buf[2360], buf[2361]);
	return r;
}

/* v0.47: SET CD SPEED, to see whether a CD copy can run faster than the 4x the drive reads at by
 * itself (11-23: 0.63 - 0.70 MB/s whether one sector or 256 are asked for at a time).
 *   The packet: Linux drivers/scsi/sr_ioctl.c sr_select_speed(): opcode 0xBB (GPCMD_SET_SPEED), the
 *     read speed in kB/s in bytes 2-3 (0xFFFF = the drive's highest), every other byte 0, no data
 *     (DMA_NONE). On a PS3 running Linux that packet reaches this drive through Sony's own driver,
 *     drivers/scsi/ps3rom.c ps3rom_atapi_request(): any command but READ(10) / WRITE(10) is passed
 *     to the drive as it is, with pktlen 12 and, when there is no data, proto NON_DATA_PROTO (0).
 *   How a no-data command goes through syscall 604 from GameOS: [I] source/psx.c L2794-2820
 *     load_unload_bdvd() (START STOP UNIT): the five words 12, 0, 0, 0 (ATAPI_NON_DATA_PROTO), 1,
 *     no buffer, length 0.
 * So the bytes and the way of sending are each from working code for this drive, but no one source
 * sends exactly this from GameOS: it asks before its first run like every other command. */
s32 drv_set_cd_speed(u32 fd, u32 kbs)
{
	cmd56 c;
	s32 r;

	memset(&c, 0, sizeof c);
	c.pkt[0] = 0xBB;
	c.pkt[2] = (u8)(kbs >> 8);
	c.pkt[3] = (u8)kbs;
	LOG("drive: next is SET CD SPEED (syscall 604, opcode 0xBB, no data) to %u kB/s", (unsigned)(kbs & 0xFFFF));
	r = send_as(fd, &c, NULL, 0, 0, 0);
	LOG("  SET CD SPEED: result %x", (u32)r);
	return r;
}
