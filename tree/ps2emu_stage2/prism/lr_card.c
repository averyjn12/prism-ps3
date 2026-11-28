/* LR-G3b .. LR-G5: the trigger file, the bring-up ladder, and the disc-read switch.
 *
 * /dev_hdd0/tmp/prism_lr.cfg (text, 16 to 512 bytes, written by the bench script) decides how far
 * this build goes. Without it the build is the read-only probe and writes no USB register.
 *
 *   PRISMLR1
 *   mode=probe | enum | msc | iso | menu
 *   path=/PS2ISO/TINYTEES.ISO        (iso mode: file on the card, / or \ separators)
 *   lun=N                            (optional: use this LUN instead of the first ready one)
 *   keep=1                           (optional: do not disarm the file after reading it)
 *   cmd=N                            (menu mode: a read of sector N of the mounted title switches to the card file)
 *
 * The file is ONE-SHOT: its first byte is overwritten once it has been read, so a stale
 * trigger cannot redirect a later, ordinary launch.
 *
 * LR-G6: with no armed trigger file, the same text is looked for inside the mounted hard-disk
 * title (see cfg_load). That is how a game is launched from the XMB with no PC.
 *
 * Whatever fails, the carrier ISO that Cobra mounted from the hard disk still boots: this
 * file only switches the read path once a file on the card is open and its first sectors
 * have been read back.
 */
#include <ps2emu/ps2emu.h>
#include <ps2emu/libc.h>
#include <ps2emu/io.h>
#include <ps2emu/symbols.h>
#include "lr.h"

#define CFG_SZ		512
#define MODE_PROBE	0
#define MODE_ENUM	1
#define MODE_MSC	2
#define MODE_ISO	3
#define MODE_MENU	4	/* LR-MENU: keep the mounted title until its program reads the command sector */
#define BENCH_BYTES	(32u << 20)
#define MAX_RECOVER	3
#define LR_STAT_MS	60000ULL	/* tail record period: one minute */

static struct
{
	u8 valid, mode, keep;
	int lun;
	u32 cmd;	/* menu mode: sector of the mounted title whose read means "switch now" */
	char path[200];
} cfg;

static u8 card_ready, card_on, dead, switching;
static u8 menu_on, pin_ok;	/* LR-MENU: a launcher title is running; its shortcut queue exists */
static u32 n_pin, n_act, last_act;
static int card_r = LR_E_STATE, last_r, n_games;
static int art_fd = -1;		/* LR_ART_FILE on the hard disk, open while the launcher runs */
static u32 art_secs;
static int carrier_fd = -1;
static void pin_reset(int fd);
static u64 card_size;
static u32 saved_blocks;
static u64 t_serve;

static struct
{
	u32 reads, errs, recov, max_us;
	u64 bytes, tb;
} st;

static int starts(const char *s, const char *key)
{
	while (*key)
	{
		if (*s++ != *key++)
			return 0;
	}

	return 1;
}

/* Read one config text (trigger file or shortcut marker) and parse it. Returns 1 if it starts
 * with the magic. */
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
		char *v;

		for (end = p; *end && *end != '\n'; end++)
			;
		v = end;
		if (*end)
			*end++ = 0;
		while (v > p && (v[-1] == '\r' || v[-1] == ' '))
			*--v = 0;

		if (starts(p, "mode="))
		{
			p += 5;
			cfg.mode = starts(p, "menu") ? MODE_MENU : starts(p, "iso") ? MODE_ISO : starts(p, "msc") ? MODE_MSC :
				starts(p, "enum") ? MODE_ENUM : MODE_PROBE;
		}
		else if (starts(p, "path="))
		{
			int i;

			for (i = 0; p[5 + i] && i < (int)sizeof(cfg.path) - 1; i++)
				cfg.path[i] = p[5 + i];
			cfg.path[i] = 0;
		}
		else if (starts(p, "lun=") && p[4] >= '0' && p[4] <= '7')
			cfg.lun = p[4] - '0';
		else if (starts(p, "keep=1"))
			cfg.keep = 1;
		else if (starts(p, "cmd="))
		{
			for (p += 4, cfg.cmd = 0; *p >= '0' && *p <= '9'; p++)
				cfg.cmd = cfg.cmd * 10 + (u32)(*p - '0');
		}
	}

	return 1;
}

/* path=* in a marker: the game is the file of the mounted title's own name, in LR_GAME_DIR on
 * the card. A shortcut made on the console is a copy of one template under the game's name, so
 * nothing in it has to be written per game. The title's path is the text at byte 1 of Cobra's
 * /tmp/cfg.bin (common.c setup_iso). */
static void path_from_title(void)
{
	char b[257];
	int fd = ufs_open(0, "/tmp/cfg.bin"), i, s = 0;

	cfg.path[0] = 0;
	if (fd < 0)
		return;

	memset(b, 0, sizeof(b));
	ufs_read(fd, 0, b, sizeof(b) - 1);
	ufs_close(fd);

	for (i = 1; b[i]; i++)
	{
		if (b[i] == '/')
			s = i + 1;
	}
	if (s)
		lr_fmt(cfg.path, sizeof(cfg.path), LR_GAME_DIR "/%s", b + s);
}

/* Two sources, in this order:
 *   1. the one-shot trigger file (bench runs; written from the PC);
 *   2. LR-G6: a marker in the mounted hard-disk title itself, 512 bytes of the same text at
 *      LR_MARK_OFF (sector 15, inside the ISO9660 system area). A "shortcut" image made by
 *      bench/lr_title.py carries one; an ordinary game image does not start that sector with
 *      the magic. The marker is never written to. */
static void cfg_load(void)
{
	const char *src = "trigger";
	int fd = ufs_open(0, LR_CFG_FILE);

	cfg.valid = 0;
	cfg.mode = MODE_PROBE;
	cfg.lun = -1;
	cfg.cmd = 0;
	cfg.path[0] = 0;

	if (fd >= 0)
	{
		cfg.valid = (u8)cfg_read(fd, 0, lr_file_size(fd));
		if (cfg.valid && !cfg.keep)
		{
			char x = 'x';

			ufs_write(fd, 0, &x, 1);
		}
		ufs_close(fd);
		if (!cfg.valid)
			lr_log("cfg: present but not armed (short, unreadable or already used)\n");
	}

	if (!cfg.valid && carrier_fd >= 0)
	{
		src = "shortcut";
		cfg.valid = (u8)cfg_read(carrier_fd, LR_MARK_OFF, CFG_SZ);
		if (cfg.valid && cfg.path[0] == '*')
			path_from_title();
	}

	if (!cfg.valid)
	{
		if (fd < 0)
			lr_log("cfg: none, read-only probe\n");
		return;
	}

	lr_log("cfg: %s mode %d lun %d keep %d path %s\n", src, cfg.mode, cfg.lun, cfg.keep, cfg.path);
}

static int bring_up(int first)
{
	int r;

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
	if (r || cfg.mode < MODE_MSC)
		return r;

	/* first: 1 = stage2 entry (a card may need seconds to power up), 2 = the launcher asked to
	 * look again (short wait: this runs inside a disc read) */
	return lr_msc_select(first ? cfg.lun : lr_lun, first == 1 ? LR_MSC_WAIT_FIRST : LR_MSC_WAIT_AGAIN);
}

#if !defined(PS2HWEMU)	/* A01 2026-11-27: the 32 MB timed read (mode=msc) is the old field kit's step 2 only; the
 * kit carries its own helper, and the room it took holds set_addr() (lr_usb.c) */
static void bench(void)
{
	u8 *s = lr_dma + LR_OFF_DATA;
	u32 lba = 0, n = LR_DATA_SZ / 512, total = BENCH_BYTES / 512;
	u64 t0, dt;
	int r;

	r = lr_blk_read(0, 1, LR_OFF_DATA);
	if (r)
	{
		lr_log("g4: FAIL sector 0 read %d\n", r);
		return;
	}
	lr_log("g4: sector 0 ends %02x %02x, first bytes %02x %02x %02x %02x\n", s[510], s[511], s[0], s[1], s[2], s[3]);

	if (total > lr_blk_count)
		total = lr_blk_count;

	t0 = lr_tb();
	while (lba < total)
	{
		if (n > total - lba)
			n = total - lba;
		r = lr_blk_read(lba, n, LR_OFF_DATA);
		if (r)
		{
			lr_log("g4: FAIL read at lba %x: %d\n", lba, r);
			return;
		}
		lba += n;
	}
	dt = (lr_tb() - t0) / LR_TB_PER_MS;
	if (!dt)
		dt = 1;

	lr_log("g4: PASS %d KB in %d ms = %d KB/s\n", (int)(total / 2), (int)dt, (int)((u64)(total / 2) * 1000 / dt));
}
#endif

/* gxemu/main.c hands over the open hard-disk title before lr_init(). */
void lr_carrier(int fd)
{
	carrier_fd = fd;
}

/* Title ID of the card file that is open ("SLUS_209.46"): the boot file its SYSTEM.CNF names.
 * Empty for an image with no ISO9660 volume or no such line. b = 2048 bytes to work in.
 * Returns the disc kind by the rule disc_type() uses: 0 DVD, 1 CD, 2 raw 2352-byte CD image. */
static int game_id(char *out, u8 *b)
{
	u32 i;
	char *p;
	int kind;

	memset(out, 0, 12);
	if (lr_file_read(0x8000, b, 0xB0))
		return 0;
	if (b[0] != 1 || memcmp(b + 1, "CD001", 5) != 0)
		return 2;
	kind = (*(u32 *)(b + 0xAA) == 0x800);
	if (lr_file_read((u64)lr_le32(b + 158) * 2048, b, 2048))
		return kind;

	/* the root directory's first sector: records of b[i] bytes, name length at +32, name at +33 */
	for (i = 0; i + 44 < 2048 && b[i]; i += b[i])
	{
		if (b[i + 32] >= 10 && memcmp(b + i + 33, "SYSTEM.CNF", 10) == 0)
			break;
	}
	if (i + 44 >= 2048 || !b[i])
		return kind;

	if (lr_file_read((u64)lr_le32(b + i + 2) * 2048, b, 512))
		return kind;
	b[511] = 0;
	for (p = (char *)b; *p; p++)
	{
		if (starts(p, "cdrom0:\\"))
		{
			p += 8;
			for (i = 0; i < 11 && p[i] > ' ' && p[i] != ';'; i++)
				out[i] = p[i];
			return kind;
		}
	}

	return kind;
}

/* After a listing: read every game's title ID, and leave the list on the hard disk for the PC
 * (LR_GAMES_FILE, in place, only if it is there and long enough; one empty record ends it). */
static void game_ids(void)
{
	static u8 *b;
	u8 rec[128];
	char id[12];
	u64 size;
	int i, fd;

	if (!b)
		b = malloc(2048);
	if (!b)
		return;

	fd = ufs_open(0, LR_GAMES_FILE);
	for (i = 0; i <= n_games; i++)
	{
		if (i < n_games)
		{
			int kind = 0;

			memset(id, 0, sizeof(id));
			if (lr_fs_cat_open((u32)i, &size, 1) == 0)
				kind = game_id(id, b);
			lr_fs_cat_id((u32)i, id, kind);
		}
		lr_fs_cat_rec((u32)i, rec);
		if (fd >= 0 && lr_file_size(fd) >= (u64)(i + 1) * 128)
			ufs_write(fd, (u64)i * 128, rec, 128);
	}
	if (fd >= 0)
		ufs_close(fd);
}

/* Reader, card, file system; then the one file (a shortcut) or the list of games (a launcher).
 * Returns 0 with card_ready set, or why not. */
static int card_up(int first)
{
	int r;

	card_ready = 0;
	if (!lr_emu_usb_started())
	{
		lr_log("card: the emulator's USB is not up at stage2 entry; nothing done\n");
		return LR_E_STATE;
	}

	r = bring_up(first);
	lr_log("card: bring-up %d\n", r);
	lr_log_flush();
	if (r || cfg.mode < MODE_MSC)
		return r;

	saved_blocks = lr_blk_count;

	if (cfg.mode == MODE_MSC)
	{
#if !defined(PS2HWEMU)
		bench();
#endif
		return 0;
	}

	r = lr_fs_mount();
	if (r == 0)
		r = menu_on ? lr_fs_list() : lr_fs_open(cfg.path, &card_size);
	if (r < 0)
	{
		lr_log("card: FAIL mount/open %d; the carrier disc will run\n", r);
		return r;
	}

	if (menu_on)
	{
		n_games = r;
		game_ids();
		lr_log("menu: %d game(s) in " LR_GAME_DIR " and " LR_GAME_DIR2 "%s\n", r, lr_cat_more ? " (more not listed)" : "");
	}
	card_ready = 1;

	return 0;
}

void lr_card_init(void)
{
	cfg_load();
	lr_log_flush();
	if (!cfg.valid || cfg.mode == MODE_PROBE)
		return;

	if (cfg.mode == MODE_MENU && cfg.cmd)
	{
		int fd = ufs_open(0, LR_PIN_FILE);

		/* on even with no card: the launcher's program reads the state and says so */
		menu_on = 1;
		if (fd >= 0)
		{
			pin_ok = lr_file_size(fd) >= (LR_PIN_MAX + 1) * LR_PIN_REC;
			if (pin_ok)
				pin_reset(fd);
			ufs_close(fd);
		}
		if (pin_ok)
		{
			/* both templates must be there, or a queued copy would make nothing */
			int t1 = ufs_open(0, "/tmp/prism_tpl_dvd.iso"), t2 = ufs_open(0, "/tmp/prism_tpl_cd.iso");

			pin_ok = t1 >= 0 && t2 >= 0;
			if (t1 >= 0)
				ufs_close(t1);
			if (t2 >= 0)
				ufs_close(t2);
		}

		art_fd = ufs_open(0, LR_ART_FILE);
		if (art_fd >= 0)
		{
			u64 n = lr_file_size(art_fd) / 2048;

			art_secs = n > LR_MENU_ART_N ? LR_MENU_ART_N : (u32)n;
		}
	}

	card_r = card_up(1);
}

/* Disc type of the card file that is open: Cobra's rule for a 2048-byte image, and anything
 * without an ISO9660 volume descriptor is a raw 2352-byte CD image. */
static int disc_type(int *is_cd, int *is_raw)
{
	u8 pvd[0xB0];

	if (lr_file_read(0x8000, pvd, sizeof(pvd)))
		return LR_E_STATE;

	if (pvd[0] == 1 && memcmp(pvd + 1, "CD001", 5) == 0)
	{
		/* same rule as Cobra's mount_ps2_discfile(): root directory size 0x800 = CD */
		*is_cd = (*(u32 *)(pvd + 0xAA) == 0x800);
		*is_raw = 0;
	}
	else
	{
		*is_cd = 1;
		*is_raw = 1;	/* 2352-byte sectors, one data track */
	}

	return 0;
}

/* Queue one change to the XMB for game i: a script line for webMAN, written over the start of
 * a record that is all comment (so what is left of the record stays a comment line).
 *   no shortcut yet      copy <template>=LR_PIN_DIR/<the game's file name>      returns 0
 *   a shortcut is there  del LR_PIN_DIR/<the game's file name>                  returns 1
 * Every line names a file in LR_PIN_DIR, a folder that holds nothing but shortcuts, so no line
 * can reach a real title whatever webMAN makes of it (2026-11-21: its "copy" replaced a real
 * title when a guarding "if" was not understood; there is no "if" any more). A file in that
 * folder that does not carry our marker is left alone. Names are limited to plain characters
 * that mean nothing in a webMAN script. */
static int pin(u32 i)
{
	char rec[LR_PIN_REC], path[200], m[8];
	const char *name = lr_fs_cat_name(i), *p;
	u64 size;
	int cd, raw, fd, n, r, gone = 0;

	if (!pin_ok)
		return LR_E_PINKIT;
	if (n_pin >= LR_PIN_MAX)
		return LR_E_PINFULL;
	if (!name)
		return LR_E_NOFILE;
	for (p = name; *p; p++)
	{
		if (*p < ' ' || *p > '~' || *p == '=' || *p == ';' || *p == ',' || *p == '*' || *p == '?')
			return LR_E_PINNAME;
	}

	lr_fmt(path, sizeof(path), LR_PIN_DIR "/%s", name);
	fd = ufs_open(0, path);
	if (fd >= 0)
	{
		r = ufs_read(fd, LR_MARK_OFF, m, 8) == 8 && memcmp(m, "PRISMLR1", 8) == 0;
		ufs_close(fd);
		if (!r)
			return LR_E_PINNED;
		n = lr_fmt(rec, sizeof(rec), "del /dev_hdd0%s\n#", path);
		gone = 1;
	}
	else
	{
		r = lr_fs_cat_open(i, &size, 0);
		if (r == 0)
			r = disc_type(&cd, &raw);
		if (r)
			return r;
		n = lr_fmt(rec, sizeof(rec), "copy /dev_hdd0/tmp/prism_tpl_%s.iso=/dev_hdd0%s\n#", cd ? "cd" : "dvd", path);
	}

	fd = ufs_open(0, LR_PIN_FILE);
	if (fd < 0)
		return LR_E_PINKIT;
	ufs_write(fd, (u64)n_pin * LR_PIN_REC, rec, n);
	ufs_write(fd, (u64)LR_PIN_MAX * LR_PIN_REC, "/refresh.ps3\n#", 14);
	ufs_close(fd);
	n_pin++;

	return gone;
}

/* A launcher start: put the queue back to all comment. What the last session queued was acted
 * on when the XMB came up in between; a line left standing would be acted on again. */
static void pin_reset(int fd)
{
	char rec[LR_PIN_REC];
	int i;

	for (i = 0; i < LR_PIN_REC; i++)
		rec[i] = (i & 63) == 0 ? '#' : (i & 63) == 63 ? '\n' : '.';
	for (i = 0; i <= LR_PIN_MAX; i++)
		ufs_write(fd, (u64)i * LR_PIN_REC, rec, LR_PIN_REC);
}

/* ---- a disc change the emulator can see (EXPERIMENTAL, see lr.h) ---- */
static u8 swap;			/* 0 none, 1 "removed" still to be said, 2 no disc */
static u8 swap_polls;
static u64 t_swap;

/* The card file that is open becomes the disc. */
static void swap_finish(const char *how)
{
	swap = 0;
	switching = 1;
	lr_take_card();
	if (!card_on)
	{
		switching = 0;
		last_r = LR_E_STATE;
	}
	lr_log("swap: %s, card on %d\n", how, card_on);
	lr_atapi_log("swap");
	lr_atapi_watch(48);
	lr_log_flush();
}

/* ---- a disc swap in mid-game (LR-MENU-CD) ----
 * Held on the pad in port 1: Select + L3 + R3. The tray opens (as for a game started from the
 * launcher), the NEXT DISC OF THE SET becomes the disc, the tray closes. The next disc is the
 * file of the same name with the number after "Disc " counted up by one, or disc 1 again after
 * the last. With no such file the same disc goes back in. */
static u8 swap_mid, combo_held;

static u32 pad_buttons(void)
{
#ifdef GX_PAD_TABLE
	u8 *dev = *(u8 **)(u64)GX_PAD_TABLE, *blk;

	if (!dev)
		return 0;
	blk = dev + GX_PAD_BLOCK + GX_PAD_BLOCK_SZ * *(u8 *)(u64)(GX_PAD_TABLE + 0x10);

	return blk[0] ? ((u32)blk[2] << 8) | blk[3] : 0;
#else
	return 0;	/* this emulator's pad table is not known yet: no mid-game swap */
#endif
}

/* Where in a path the disc number is: the digit after "Disc " / "disc " / "Disk ". -1 = none. */
static int disc_digit(const char *p)
{
	int i, at = -1;

	for (i = 0; p[i] && p[i + 5]; i++)
	{
		if ((p[i] == 'D' || p[i] == 'd') && p[i + 1] == 'i' && p[i + 2] == 's' && (p[i + 3] == 'c' || p[i + 3] == 'k') && p[i + 4] == ' '
			&& p[i + 5] >= '1' && p[i + 5] <= '9')
			at = i + 5;
	}

	return at;
}

/* The tray is about to close: put the next disc in. */
static void swap_next(void)
{
	int at = disc_digit(cfg.path), r = LR_E_NOFILE;
	char was = at >= 0 ? cfg.path[at] : 0;

	swap = 0;
	swap_mid = 0;
	if (at >= 0)
	{
		cfg.path[at] = (char)(was + 1);
		r = was < '9' ? lr_fs_open(cfg.path, &card_size) : LR_E_NOFILE;
		if (r && was != '1')
		{
			cfg.path[at] = '1';
			r = lr_fs_open(cfg.path, &card_size);
		}
		if (r)
			cfg.path[at] = was;
	}
	if (r)
	{
		/* no other disc: the same one goes back in */
		r = lr_fs_open(cfg.path, &card_size);
		lr_log("swap: no other disc of the set; the same disc is back in (%d)\n", r);
		if (r)
		{
			dead = 1;
			return;
		}
	}
	lr_take_card();
	lr_log("swap: disc now %s\n", cfg.path);
	lr_atapi_log("swap");
	lr_atapi_watch(48);
	lr_log_flush();
}

/* What the emulator does with a media poll, read in ps2_gxemu (Evilnat 4.93) on 2026-11-22:
 * the routine at 0x8E850 sends GET EVENT STATUS NOTIFICATION (class 0x10) and returns BIT 0 OF
 * BYTE 5 of the answer, "tray open", and nothing else (not the event code, not "media present");
 * its caller at 0x8C9D0 keeps that bit as the PS2's tray state and notes the time when it
 * changes. Build k answered "removal" / "new media" codes with that bit clear, so the emulator
 * never saw anything move (console run C1). Here the tray is OPEN (byte 5 = 1) for at least
 * LR_TRAY_POLLS polls and LR_TRAY_MS, then the card file becomes the disc and the tray is
 * CLOSED again (byte 5 = 2, media present). */
int lr_event(const u8 *cmd, u8 *out)
{
	u8 status = 1;		/* tray open, no disc */

	if (!swap || cmd[0] != 0x4A || cmd[4] != 0x10 || !out)
		return 0;

	if (swap == 1)
	{
		swap = 2;
		swap_polls = 0;
	}
	if (++swap_polls > LR_TRAY_POLLS && lr_tb() - t_swap > LR_TRAY_MS * LR_TB_PER_MS)
	{
		if (swap_mid)
			swap_next();
		else
			swap_finish("tray closed on the new disc");
		status = 2;	/* tray closed, media present */
	}

	/* event header (length 4, class 4 = media, classes supported as Cobra says), then the event */
	memset(out, 0, 8);
	out[1] = 4;
	out[2] = 4;
	out[3] = 0x0F;
	out[4] = status == 1 ? 1 : 2;	/* eject request / new media: not read by this emulator */
	out[5] = status;
	if (swap_polls <= 2 || status == 2)
		lr_log("swap: media poll %d answered status %d\n", swap_polls, status);
	if (status == 2)
		lr_log_flush();

	return 1;
}

/* Called on every disc command: if the emulator never asked for a media event, do what build j
 * did, the plain switch. */
void lr_swap_tick(void)
{
	if (swap && lr_tb() - t_swap > (swap_mid ? LR_SWAP_MID_MS : LR_SWAP_MS) * LR_TB_PER_MS)
	{
		if (swap_mid)
		{
			/* nothing was shown to the game: leave its disc alone */
			swap = 0;
			swap_mid = 0;
			lr_log("swap: no media poll in time, not swapped\n");
		}
		else
			swap_finish("no media poll in time, plain switch");
	}

	/* a game is running from the card: is the swap combination held? */
	if (card_on && !dead)
	{
		int held = (pad_buttons() & LR_SWAP_COMBO) == LR_SWAP_COMBO;

		if (held && !combo_held && !swap)
		{
			swap = 1;
			swap_mid = 1;
			t_swap = lr_tb();
			lr_atapi_mark("disc swap asked");
			lr_log("swap: asked on the pad while %s is in\n", cfg.path);
			lr_log_flush();
		}
		combo_held = (u8)held;
	}
}

/* One action asked for by the launcher's program (see lr.h). */
static void menu_act(u32 k)
{
	n_act++;
	last_act = k;

	if (swap)
		last_r = LR_E_STATE;	/* a disc change is under way */
	else if (k == LR_ACT_RESCAN)
	{
		lr_fs_forget();
		card_r = card_up(2);
		last_r = card_r;
	}
	else if (!card_ready)
		last_r = LR_E_NOMEDIUM;
	else if (k >= LR_ACT_PIN)
		last_r = pin(k - LR_ACT_PIN);
	else
	{
		const char *name = lr_fs_cat_name(k);

		last_r = lr_fs_cat_open(k, &card_size, 0);
		if (last_r == 0)
		{
			int cd = 0, raw = 0;

			lr_fmt(cfg.path, sizeof(cfg.path), LR_GAME_DIR "/%s", name);
			if (disc_type(&cd, &raw) == 0 && (cd || card_size > (u64)LR_MENU_SECTORS * 2048))
			{
				/* not the single-layer DVD the launcher title said it was: a disc change */
				swap = 1;
				t_swap = lr_tb();
				lr_atapi_mark("swap asked");
				lr_log("swap: asked for %s cd %d raw %d size %lx\n", name, cd, raw, (unsigned long)card_size);
			}
			else
			{
				/* From the next read on, the disc is this file. */
				switching = 1;
				lr_take_card();
				if (!card_on)
				{
					switching = 0;
					last_r = LR_E_STATE;
				}
			}
		}
	}

	lr_log("menu: action %u result %d\n", k, last_r);
	lr_log_flush();
}

/* A read of the launcher title while its menu runs. Outside the command file it is a plain
 * read of the image. */
static int menu_read(int fd, u64 off, void *buf, u64 len)
{
	u64 base = (u64)cfg.cmd * 2048;
	s64 rel, last;
	u32 k, i, n;
	u8 *p = buf;

	if ((off & 2047) || (len & 2047) || off + len <= base || off >= base + (u64)LR_MENU_END * 2048)
		return ufs_read(fd, off, buf, len);

	/* part of the read may lie before the file; past the image's end nothing comes back */
	memset(buf, 0, len);
	if (off < base)
		ufs_read(fd, off, buf, base - off);

	n = (u32)(len / 2048);
	rel = (s64)(off / 2048) - (s64)cfg.cmd;
	last = rel + n - 1;

	if (rel >= LR_MENU_ART)
	{
		/* cover art and titles: straight from the hard-disk file */
		if (art_fd >= 0 && rel - LR_MENU_ART < art_secs)
			ufs_read(art_fd, (u64)(rel - LR_MENU_ART) * 2048, buf, len);
		return (int)len;
	}

	/* the first action sector at or after the start of the read */
	k = rel <= LR_MENU_ACT ? 0 : (u32)(rel - LR_MENU_ACT + LR_MENU_STEP - 1) / LR_MENU_STEP;
	if (k < LR_ACT_N)
	{
		s64 c = LR_MENU_ACT + (s64)k * LR_MENU_STEP;

		if (c <= last && c - rel <= 16)
		{
			menu_act(k);
			return (int)len;
		}
	}

	for (i = 0; i < n; i++, p += 2048)
	{
		s64 s = rel + i;

		if (s == 0)
			lr_fmt((char *)p, 2048, "PRISMCAT1\nver=" LR_VERSION "\ncard=%d\ncount=%d\nmore=%d\npin=%d\npins=%u\nseq=%u\nact=%u\nlast=%d\nart=%u\n",
				card_ready ? 0 : card_r, card_ready ? n_games : 0, lr_cat_more, pin_ok, n_pin, n_act, last_act, last_r, art_secs);
		else if (s >= LR_MENU_CAT && s < LR_MENU_CAT + LR_MENU_CAT_N && card_ready)
		{
			for (k = 0; k < 16; k++)
			{
				u8 *rec = p + 128 * k;
				char path[160];
				int hd;

				lr_fs_cat_rec((u32)(s - LR_MENU_CAT) * 16 + k, rec);
				if (!rec[0])
					continue;

				/* [1] = the shortcut folder on the hard disk has a file of this name */
				lr_fmt(path, sizeof(path), LR_PIN_DIR "/%s", (char *)rec + 8);
				hd = ufs_open(0, path);
				if (hd >= 0)
				{
					rec[1] = 1;
					ufs_close(hd);
				}
			}
		}
	}

	return (int)len;
}

/* Called by gxemu/main.c once Cobra has set up the carrier ISO. Returns 1 when the card file
 * is to be served instead, with its size and disc type; 2 when a launcher title is to keep
 * running for now (menu mode) and must report a full-size disc; 0 otherwise. */
int lr_iso_take(u64 *size, int *is_cd, int *is_raw)
{
	if (menu_on && !switching)
	{
		/* the mounted title runs first; lr_iso_read() calls back here when its program asks */
		lr_log("menu: launcher title runs, command file at sector %u\n", cfg.cmd);
		return 2;
	}

	if (!card_ready)
		return 0;

	if (disc_type(is_cd, is_raw))
	{
		lr_log("card: FAIL first read of the file; the carrier disc will run\n");
		return 0;
	}

	*size = card_size;
	card_on = 1;
	t_serve = lr_tb();
	lr_log("card: SERVING %s size %lx cd %d raw %d\n", cfg.path, (unsigned long)card_size, *is_cd, *is_raw);
	lr_atapi_mark("card taken");
	lr_log_flush();

	return 1;
}

/* Replaces ufs_read(iso_fd, ...) in Cobra's common.c. */
int lr_iso_read(int fd, u64 off, void *buf, u64 len)
{
	u64 t0;
	u32 us;
	int r, tries;

	if (!card_on)
	{
		if (menu_on)
			return menu_read(fd, off, buf, len);

		return ufs_read(fd, off, buf, len);
	}

	st.reads++;
	st.bytes += len;

	if (dead)
	{
		memset(buf, 0, len);
		return (int)len;
	}

	t0 = lr_tb();
	r = lr_file_read(off, buf, (u32)len);

	for (tries = 0; r && tries < MAX_RECOVER; tries++)
	{
		st.errs++;
		if (st.errs <= 8)
			lr_log("card: read off %lx len %x failed %d, recovering (%d)\n", (unsigned long)off, (int)len, r, tries + 1);

		lr_fs_forget();
		r = bring_up(0);
		if (r == 0 && lr_blk_count != saved_blocks)
		{
			lr_log("card: a different medium came back (%u blocks, was %u)\n", lr_blk_count, saved_blocks);
			r = LR_E_STATE;
			break;
		}
		if (r == 0)
		{
			st.recov++;
			r = lr_file_read(off, buf, (u32)len);
		}
	}

	if (r)
	{
		/* The card is gone or broken. Hand back zeroes from here on: the game sees a bad
		 * disc, the console does not hang. */
		dead = 1;
		memset(buf, 0, len);
		lr_log("card: DEAD after read off %lx (%d); serving zeroes\n", (unsigned long)off, r);
		lr_log_flush();
		return (int)len;
	}

	t0 = lr_tb() - t0;
	st.tb += t0;
	us = (u32)(t0 * 1000 / LR_TB_PER_MS);
	if (us > st.max_us)
		st.max_us = us;

	return (int)len;
}

void lr_card_stats(const char *tag)
{
	if (!card_on)
		return;

	lr_log("card: %s reads %u bytes %lx errs %u recov %u max %u us total %u ms dead %d\n", tag, st.reads,
		(unsigned long)st.bytes, st.errs, st.recov, st.max_us, (u32)(st.tb / LR_TB_PER_MS), dead);
	lr_atapi_log(tag);
}

/* Called from lr_poll(): every LR_STAT_MS of play, rewrite the tail record of the log file
 * with the running totals. */
void lr_card_tick(void)
{
	static u64 next;
	u64 now;

	if (!card_on)
		return;

	now = lr_tb();
	if (now < next)
		return;
	next = now + LR_STAT_MS * LR_TB_PER_MS;
	{
		/* the first few tail records also go to the log itself, with the command counts */
		static u8 n;

		if (n < 4)
		{
			n++;
			lr_card_stats("tail");
			lr_log_flush();
		}
	}

	lr_log_tail("card: tail at %u s reads %u bytes %lx errs %u recov %u max %u us total %u ms dead %d\n",
		(u32)((now - t_serve) / (LR_TB_PER_MS * 1000)), st.reads, (unsigned long)st.bytes, st.errs, st.recov,
		st.max_us, (u32)(st.tb / LR_TB_PER_MS), dead);
}
