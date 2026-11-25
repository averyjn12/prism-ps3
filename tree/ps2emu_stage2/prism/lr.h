/* PlayStation Prism S36 "BC live read" - stage2 instrumentation (LR-G1 .. LR-G3).
 *
 * Built into Cobra's ps2gxemu_stage2.bin when the Makefile is given LR_GATE=1 or LR_GATE=3.
 *   LR_GATE=1  read-only probe. Contains NO store to any USB register (lr_ehci.c is not linked).
 *   LR_GATE=3  probe + the dev0-EHCI takeover and one GET_DESCRIPTOR(device).
 *   LR_GATE=5  probe + the card reader on the emulator's own controller, as far as the trigger
 *              file /dev_hdd0/tmp/prism_lr.cfg asks (see lr_card.c). No trigger = read-only.
 *
 * Target: CECHE01 mule, Evilnat 4.93 Cobra 8.5, ps2_gxemu.self (kozarovv build). Every fixed
 * address below belongs to THAT binary and is guarded by lr_sig_check() before use.
 */
#ifndef PRISM_LR_H
#define PRISM_LR_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  s64;

#if defined(PS2HWEMU)
#define LR_VERSION	"2026-11-24-a01k11"	/* A01 field kit 1.1 (app_a01/): + wide emulator check (any fw 4.84-4.93) */
#else
#define LR_VERSION	"2026-11-22q"
#endif

/* ---- Which emulator: the Makefile's -DPS2GXEMU (CECHC/E, the mule) or -DPS2HWEMU (CECHA/B,
 *      ps2_emu; 2026-11-23, NOT YET RUN ON A CONSOLE). The GX_ names below mean "the emulator's"
 *      in both builds, so the gxemu build is unchanged; HW_ names exist only in the hwemu build.
 *      hwemu sources: lr/re_notes/ps2_emu_ehci_coop_2026-11-23.md. ---- */
#if defined(PS2GXEMU)
#define LR_EMU_NAME		"gxemu"
/* ---- gxemu (Evilnat 4.93) addresses, all VADDRs. Sources: re/tools decomp of 0x0413A8,
 *      0x06D2C0, 0x070F90 (2026-11-19) and EMU handoff #4. ---- */
#define GX_SIG1_ADDR		0x070F90	/* EHCI start: lis r11,0x6030 */
#define GX_SIG1_WORD		0x3d606030
#define GX_SIG2_ADDR		0x0413A8	/* USB attach: mfcr r12 */
#define GX_SIG2_WORD		0x7d800026
#define GX_USB_DEVTAB		0x329170	/* 2 entries x 96 bytes */
#define GX_EHCI_CAPBASE_PTR	0x1775240	/* u64: cap base the emulator's EHCI driver uses */
#define GX_EHCI_OPBASE_PTR	0x1774EE0	/* u64: its op base; 0 until the emulator's USB init ran */
#define GX_EHCI_NPORTS		0x1775238	/* u8  */
#define GX_FILETAB_PTR_TOC	(-0xE68)	/* TOC slot -> ufs file table (0x1270 per fd) */
/* The pad in each PS2 port, as the pad-command routine 0x7F1EC finds it (read 2026-11-22,
 * re_notes/gxemu_disc_tray_2026-11-22.md): a table of 0x18-byte entries, +0 the device object
 * (0 = none), +0x10 which pad of that device; in the object the pads' blocks start at +0x40,
 * 0x34 bytes each: [0] non-zero = a pad is there, [2] [3] its button bytes, 1 = pressed, in
 * the PS2's order (0x01 Select, 0x02 L3, 0x04 R3, 0x08 Start ... / L2 R2 L1 R1 ...). */
#define GX_PAD_TABLE		0x17752F0
#define GX_PAD_BLOCK		0x40
#define GX_PAD_BLOCK_SZ		0x34

#elif defined(PS2HWEMU)
#define LR_EMU_NAME		"hwemu"
/* ---- ps2_emu (Evilnat 4.93, re/work/ps2_emu.elf: Cobra's stage1 at 0x23D6C, TOC 0x4EB8C0) ---- */
#define GX_SIG1_ADDR		0x03C420	/* EHCI init: lis r6,0x2002 (its register window) */
#define GX_SIG1_WORD		0x3cc02002
#define GX_SIG2_ADDR		0x02A26C	/* UHC page collector: ld r6,0x70(r11) */
#define GX_SIG2_WORD		0xe8cb0070
#define HW_SIG3_ADDR		0x03CA10	/* EHCI init: ori r7,r5,0xb21 (USBCMD 0x20B21) */
#define HW_SIG3_WORD		0x60a70b21
/* Its two USB host controllers (UHC 0, 1): a table of 0xD0-byte entries, +0x70 the lpar of the
 * OHCI page, +0x78 of the EHCI page (filled by lv1_map_device_mmio_region at 0x0294AC /
 * 0x0294E0). 0x01D2E0 maps the four pages at FIXED EAs: OHCI 0x20012000 / 0x20013000,
 * EHCI 0x20022000 / 0x20023000. Its EHCI driver is hard-wired to UHC 1 (0x20023000). */
#define HW_UHC_TAB		0x505BD0
#define HW_UHC_SZ		0xD0
#define HW_EA_OHCI(i)		(0x20012000ULL + 0x1000ULL * (u64)(i))
#define HW_EA_EHCI(i)		(0x20022000ULL + 0x1000ULL * (u64)(i))
#define HW_LPAR_OHCI(i)		(*(u64 *)(u64)(HW_UHC_TAB + HW_UHC_SZ * (i) + 0x70))
#define HW_LPAR_EHCI(i)		(*(u64 *)(u64)(HW_UHC_TAB + HW_UHC_SZ * (i) + 0x78))
#define GX_EHCI_CAPBASE_PTR	0x506118	/* u64: written by the EHCI init at 0x03C46C */
#define GX_EHCI_OPBASE_PTR	0x506068	/* u64: 0 until the EHCI init ran (0x03C474) */
#define GX_EHCI_NPORTS		0x506110	/* u8  (0x03C468) */
#define GX_FILETAB_PTR_TOC	(-0x63E0)	/* Cobra hwemu FILESZ_TOC_OFFSET: same 0x1270 / +0x178 layout */
/* no pad table yet: the mid-game swap (Select + L3 + R3) is off in this build */

#else
#error "lr.h: build with -DPS2GXEMU or -DPS2HWEMU"
#endif
#define LR_SWAP_COMBO		0x0700	/* Select + L3 + R3, in the PS2's button bytes ([2] << 8) | [3] */

/* one entry of the emulator's SB_USB device table */
typedef struct
{
	u64 dma_desc;		/* -> lr_gx_dma[n_dma] */
	u64 n_dma;
	u64 ea_ohci;		/* 0x60000000 / 0x60100000 */
	u64 ea_ehci;		/* 0x60200000 / 0x60300000 */
	u64 virq_ohci;
	u64 virq_ehci;
	u64 h_ohci;
	u64 h_ehci;
	u64 lpar_ohci;		/* non-zero once lv1_map_device_mmio_region succeeded */
	u64 lpar_ehci;
	u64 outlet_ohci;
	u64 outlet_ehci;
} lr_gx_usbdev;

typedef struct
{
	u64 name;
	u64 size;
	u64 flags;
	u64 type;
	u64 bus;
	u64 lpar;
} lr_gx_dma;

#define LR_DEVTAB	((lr_gx_usbdev *)(u64)GX_USB_DEVTAB)

/* ---- timebase ---- */
#define LR_TB_PER_MS	79800ULL	/* PS3 timebase 79.8 MHz (gxemu's own 1 ms loop adds 0x137B8) */
u64  lr_tb(void);
void lr_udelay(u32 us);
void lr_mdelay(u32 ms);

/* ---- log ---- */
#define LR_LOG_SZ	0x3000		/* RAM log (on the heap); the whole buffer is what gets written to the file */
int  lr_fmt(char *out, int size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#if defined(PS2HWEMU)
/* A01 field kit: its own files, so the E01 helper's trigger and log are never shared */
#define LR_LOG_FILE	"/tmp/prism_a01.log"	/* = /dev_hdd0/tmp/prism_a01.log, made by the kit app */
#else
#define LR_LOG_FILE	"/tmp/prism_lr.log"	/* = /dev_hdd0/tmp/prism_lr.log, must pre-exist, >= LR_LOG_SZ */
#endif
void lr_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void lr_log_flush(void);		/* write the RAM log to the file, in place */
#define LR_TAIL_OFF	0xF000		/* one record in the log FILE, rewritten in place: the latest statistics */
#define LR_TAIL_SZ	0x100
void lr_log_tail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  lr_vfmt(char *out, int size, const char *fmt, va_list ap);

/* ---- UDP mirror of the log (best effort, never blocks more than ~10 ms) ---- */
#define LR_UDP_PORT	18194
#define LR_SRC_IP	0xC0A80002	/* 192.168.0.2: put the console's own address here */
#if PRISM_LR >= 5
/* the gate-5 build has no UDP mirror: it makes no hypercall, and the window has no room */
#define lr_net_try()		((void)0)
#define lr_net_send(msg, len)	((void)0)
#else
void lr_net_try(void);
void lr_net_send(const char *msg, int len);
int  lr_net_state(void);
#endif

/* ---- probe (read-only) ---- */
int  lr_sig_check(void);
int  lr_usb_mapped(void);		/* both controllers' MMIO mapped by the emulator */
int  lr_emu_usb_started(void);		/* emulator's own USB init has completed */
void lr_repo_dump(void);
void lr_snapshot(const char *tag);
u32  lr_port_sig(void);
void lr_port_line(const char *tag);

/* ---- mmio (big-endian registers on the PS3 south bridge) ---- */
static inline u32 lr_rd(u64 base, u32 off)
{
	return *(volatile u32 *)(base + off);
}

#if PRISM_LR == 3
static inline void lr_wr(u64 base, u32 off, u32 v)
{
	*(volatile u32 *)(base + off) = v;
	__asm__ volatile("eieio");
}
void lr_ehci_run(void);
#endif

#if PRISM_LR >= 5
/* ================= LR-G3b .. G5: the card reader on the emulator's own EHCI ================= */

/* error codes shared by the layers below (0 = ok) */
#define LR_E_STALL	(-2)
#define LR_E_TIMEOUT	(-3)
#define LR_E_XACT	(-4)	/* transaction error / halted for any reason but a stall */
#define LR_E_STATE	(-5)	/* something is not as it must be; the log says what */
#define LR_E_SHORT	(-6)	/* the device sent less than asked */
#define LR_E_NOMEDIUM	(-7)
#define LR_E_NOFILE	(-8)

#if defined(PS2HWEMU)
#define LR_CFG_FILE	"/tmp/prism_a01.cfg"	/* = /dev_hdd0/tmp/prism_a01.cfg, written by the kit app */
#else
#define LR_CFG_FILE	"/tmp/prism_lr.cfg"	/* = /dev_hdd0/tmp/prism_lr.cfg */
#endif
#define LR_MARK_OFF	0x7800	/* LR-G6 marker inside a shortcut image: sector 15 */
#define LR_USB_ADDR	127	/* the emulator's own device sat at 62 on the mule; attach checks for a clash */

/* Our block: the spare end of the emulator's own EHCI DMA region (see lr_hc.c). */
#define LR_BLOCK_OFF	0x25000	/* offset inside that region */
#define LR_DMA_SZ	0xB000
#define LR_OFF_SETUP	0x0400	/* 8-byte SETUP packet */
#define LR_OFF_CBW	0x0440	/* 31 bytes */
#define LR_OFF_CSW	0x0480	/* 13 bytes */
#define LR_OFF_CTLDATA	0x0600	/* control data stage, up to LR_CTL_MAX */
#define LR_CTL_MAX	0x0200
#define LR_OFF_META	0x1000	/* directory / FAT window */
#define LR_META_SZ	0x2000
#define LR_OFF_DATA	0x3000	/* file data window: 16 disc sectors */
#define LR_DATA_SZ	0x8000

#define LR_MSC_WAIT_FIRST	50	/* x 100 ms for a card to come ready after a reset */
#define LR_MSC_WAIT_AGAIN	15

/* queue heads */
#define LR_Q_CTRL	0
#define LR_Q_OUT	1
#define LR_Q_IN		2

typedef struct
{
	u8 addr, cfg, iface, ep_in, ep_out, maxlun, subclass;
	u16 mps_in, mps_out, vid, pid;
} lr_usbdev;

extern u8 *lr_dma;		/* EA of our block, NULL until lr_hc_attach() succeeded */
extern u8 lr_swap;
extern lr_usbdev lr_dev;
extern u32 lr_blk_count;	/* 512-byte blocks on the selected LUN */
extern int lr_lun;

static inline void lr_wr(u64 base, u32 off, u32 v)
{
	*(volatile u32 *)(base + off) = v;
	__asm__ volatile("eieio");
}

/* lr_hc.c: cooperative use of the emulator's running EHCI controller */
void lr_hc_probe(const char *tag);	/* read-only: the emulator's port state and async ring */
int  lr_hc_attach(void);
int  lr_hc_port_reset(void);
void lr_hc_pipe(int q, u32 addr, u32 ep, u32 mps);
void lr_hc_toggle0(int q);
int  lr_hc_ctrl(const u8 *setup, void *data, int len);
int  lr_hc_bulk(int q, u32 off, u32 len, u32 timeout_ms);

/* lr_usb.c */
int  lr_usb_enumerate(void);
int  lr_usb_clear_halt(int q);
int  lr_msc_cmd(u32 lun, const u8 *cb, int cblen, u32 off, u32 len);
int  lr_msc_select(int want, int rounds);
int  lr_blk_read(u32 lba, u32 n, u32 off);

/* lr_fs.c (LR-SPLIT, 2026-11-22): every FAT32 / exFAT partition of the card is mounted (MBR: the
 * four primary entries; GPT: its first four), up to LR_FS_VOLS. A game is a file in LR_GAME_DIR or
 * LR_GAME_DIR2 on any of them, either whole or split: "<name>.0", "<name>.1", ... read end to end
 * as one file called <name> (every part but the last a whole number of 2048-byte sectors; the
 * FAT32 4 GB file limit). LR_GAME_DIR2 is for a FAT32 partition that webMAN can see: it never
 * lists that folder, so it never offers to copy a game from it to the hard disk.
 * lr_fs_open() takes a path on any volume; a path in LR_GAME_DIR is also looked for in
 * LR_GAME_DIR2, and each as a split game, so a shortcut finds its game wherever it lives. */
#define LR_FS_VOLS	4
#define LR_SPLIT_MAX	16		/* parts .0 .. .15 */
#define LR_GAME_DIR2	"/PS2PRISM"
int  lr_fs_mount(void);
int  lr_fs_open(const char *path, u64 *size);
int  lr_file_read(u64 off, u8 *dst, u32 len);
void lr_fs_forget(void);
#define LR_CAT_MAX	256		/* games listed, all volumes and both folders together */
extern u8 lr_cat_more;
int  lr_fs_list(void);
const char *lr_fs_cat_name(u32 i);
void lr_fs_cat_rec(u32 i, u8 *out);
int  lr_fs_cat_open(u32 i, u64 *size, int quiet);
void lr_fs_cat_id(u32 i, const char *id, int kind);
u32  lr_le32(const u8 *p);

/* ---- LR-MENU: what a launcher title's program can ask for, all of it by READING sectors of
 *      its own command file (PRISM.CMD; the marker's cmd= is that file's first sector). The file's
 *      content is made up here; what the image holds there is never returned while this is on.
 *        sector 0            state, as text lines: PRISMCAT1, ver=, card=, count=, more=, pin=,
 *                            pins=, seq=, act=, last=, art=
 *        sectors 1 .. 16     the games on the card (LR_GAME_DIR, then LR_GAME_DIR2, on each volume
 *                            in partition order), 16 records of 128 bytes each
 *                            (lr_fs_cat_rec), in directory order, each with the title ID read
 *                            from the image's SYSTEM.CNF
 *        sector 64 + 32 * k  action k. It runs when a read covers that sector and starts at most
 *                            16 sectors before it; the read returns zeroes.
 *                              k = 0 .. 255    make game k the disc (the launcher then restarts
 *                                              the PS2 side and boots it)
 *                              k = 256 .. 511  queue a shortcut title for game k - 256, or its
 *                                              removal if it has one (last = 1 then)
 *                              k = 512         look for the card again
 *        sectors 20480 ..    the bytes of the hard-disk file LR_ART_FILE (cover art and titles,
 *                            made on the PC by bench/lr_art.py), as far as it goes; art= in
 *                            sector 0 is its length in sectors
 *      After an action, sector 0 says what happened: seq counts actions, act is k, last the
 *      result (0 or an LR_E_ code).
 *      The catalogue records are also written to LR_GAMES_FILE on the hard disk (in place, if
 *      the file exists), ended by one empty record: how the PC learns what is on the card. ---- */
#define LR_GAME_DIR	"/PS2ISO"
#define LR_MENU_CAT	1
#define LR_MENU_CAT_N	(LR_CAT_MAX / 16)
#define LR_MENU_ACT	64
#define LR_MENU_STEP	32
#define LR_ACT_PIN	256
#define LR_ACT_RESCAN	512
#define LR_ACT_N	513
#define LR_MENU_ART	20480
#define LR_MENU_ART_N	16384		/* 32 MB */
#define LR_MENU_END	(LR_MENU_ART + LR_MENU_ART_N)	/* sectors in the command file */
#define LR_ART_FILE	"/tmp/prism_art.dat"
#define LR_GAMES_FILE	"/tmp/prism_games.dat"

/* Shortcut queue: a webMAN script on the hard disk, made by bench/lr_title.py as 32 records of
 * LR_PIN_REC bytes that are all comment. A queued change overwrites the start of one record with
 * ONE unconditional script line, in place (lr_card.c pin): a copy of a template into LR_PIN_DIR
 * under the game's own file name (a shortcut finds its game by its name, marker path=*), or a
 * del of such a shortcut. /dev_hdd0/onxmb.bat calls the file the next time the XMB is up; the
 * next launcher start puts it back to all comment. The stage2 itself creates no file.
 * LR_PIN_DIR holds shortcuts only: webMAN lists titles in a sub-folder of PS2ISO too. */
#define LR_PIN_DIR	"/PS2ISO/SDCARD"
#define LR_PIN_FILE	"/tmp/prism_pin.bat"
#define LR_PIN_REC	512
#define LR_PIN_MAX	31		/* record 31 is the "rescan the game list" line */
#define LR_E_PINKIT	(-20)		/* the queue file is not installed */
#define LR_E_PINNAME	(-21)		/* the name holds a character a script line cannot carry */
#define LR_E_PINFULL	(-22)
#define LR_E_PINNED	(-23)		/* a file of that name in LR_PIN_DIR is not one of our shortcuts */

/* lr_card.c */
void lr_carrier(int fd);
void lr_take_card(void);	/* in gxemu/main.c: point Cobra's disc state at the card file */
/* Menu mode: the size a launcher title reports until the switch. The emulator asks for the
 * disc's size once, at the start (console run M1: after the switch it refused a read at sector
 * 2,197,849 of a game because the launcher image had 416). This is a full single-layer DVD. */
#define LR_MENU_SECTORS	2298496u
void lr_card_init(void);
void lr_card_stats(const char *tag);
void lr_card_tick(void);
int  lr_iso_take(u64 *size, int *is_cd, int *is_raw);
/* EXPERIMENTAL (LR-MENU-CD): a game the launcher starts that is not a single-layer DVD is put
 * in as a DISC CHANGE the emulator can see: its next media-event polls (GET EVENT STATUS
 * NOTIFICATION, class 0x10) are answered "tray open" for a while, then the card file becomes
 * the disc and they are answered "tray closed, disc in" (see lr_event in lr_card.c for what
 * the emulator reads). If the polls do not come, after LR_SWAP_MS the plain switch is done. */
#define LR_SWAP_MS	8000ULL
#define LR_SWAP_MID_MS	30000ULL	/* a request in mid-game: a streaming game polls rarely (run D2) */
#define LR_TRAY_MS	1500ULL		/* the tray stays open at least this long ... */
#define LR_TRAY_POLLS	3		/* ... and for this many polls */
int  lr_event(const u8 *cmd, u8 *out);	/* 1 = this command was answered here */
void lr_swap_tick(void);
int  lr_iso_read(int fd, u64 off, void *buf, u64 len);
#endif

u64  lr_file_size(int fd);

/* ---- entry points used by gxemu/main.c ---- */
/* Every disc command the emulator sends is counted by its first byte (0x100 = it asked for the
 * disc's size), and the order of the first ones after each mark is kept: lr_atapi_mark() starts
 * a new run of 40, lr_atapi_log() writes counts and run to the log. */
void lr_atapi_op(u32 op);
void lr_atapi_mark(const char *why);
void lr_atapi_log(const char *tag);
void lr_atapi_watch(u32 n);		/* log and flush each of the next n commands (after a mid-game swap) */
int  lr_atapi_due(void);		/* the run after the last mark is full and not yet in the log */
void lr_init(void);
void lr_poll(void);

#endif
