/* v0.38: a PS1 or PS2 disc copied to an image file. Part of rip.c (included at its end: it uses the
 * device chooser, the progress line and the log there).
 *
 * How (drive.c names the source of every call):
 *   1. the console must have mounted the disc itself (wait_mounted; no drive reset, see there);
 *   2. a PS2 DVD: plain 2048-byte sector reads (syscall 602) into <name>.iso;
 *      a PS1 or PS2 CD: the track list (READ TOC), then raw 2352-byte sectors (READ CD) into
 *      <name>.bin, with a <name>.cue beside it (not on the card's exFAT part, which takes one file).
 * Where: PS2 images to /dev_hdd0/PS2ISO, the SD card's exFAT part (/PS2ISO) or its FAT32 part
 * (/PS2PRISM, as parts under 4 GiB when too big for one file) - the places Prism plays PS2 games
 * from; PS1 images to PSXISO on the SD card's FAT32 part, the hard disk or a USB drive.
 *
 * Safety (Bananafuzzbuddy's rule after v0.32 froze the console on a drive command): each KIND of drive command
 * asks before the first time it is sent on a console, one at a time (drive_step), every command is
 * announced in prism_rip.log before it is sent - the log is closed after each line, so a freeze shows
 * the last thing asked - and during the long read the log gets a line every five seconds. A command
 * that worked is noted in USRDIR/drive_ok.txt and does not ask again. */
#include <malloc.h>
#include <ppu-lv2.h>
#include "storage.h"
#include "drive.h"
#include "ripcore.h"
#include "titles.h"

#define OK_FILE		"/dev_hdd0/game/PRSM00001/USRDIR/drive_ok.txt"
#define DT_PSX_CD_	0xFF50
#define DT_PS2_CD_	0xFF60
#define FAST_CD		1	/* v0.46: off; v0.47: on again, with the drive asked for its full speed (step 4b) */

static int step_known(const char *key)
{
	char ln[64];
	FILE *f = fopen(OK_FILE, "r");
	int r = 0;

	if (!f)
		return 0;
	while (!r && fgets(ln, sizeof ln, f))
	{
		ln[strcspn(ln, " \r\n")] = 0;
		r = !strcmp(ln, key);
	}
	fclose(f);
	return r;
}

static void step_note(const char *key, const char *note)
{
	FILE *f;

	if (step_known(key))
		return;
	f = fopen(OK_FILE, "a");
	if (!f)
		return;
	fprintf(f, "%s   (%s with Prism %s)\n", key, note, APP_VERSION);
	fclose(f);
	logf_("drive check: \"%s\" %s; it will not ask again", key, note);
}

static void step_mark(const char *key)
{
	step_note(key, "worked");
}

/* v0.44: the name under which "this did not work with this build" is kept, so that the same build
 * does not ask for it again and the next build does */
static const char *step_no_key(const char *key)
{
	static char k[48];

	snprintf(k, sizeof k, "no-%s-%.4s", key, APP_VERSION);
	return k;
}

static const char *step_src = "multiMAN", *step_no = "Stop";	/* whose command it is; what Circle does */

/* 1 = send it. A command kind that has worked here before does not ask. */
static int drive_step(const char *key, const char *what)
{
	char src_ln[80];

	if (step_known(key))
		return 1;
	logf_("drive check: asking before \"%s\" (%s) - first time on this console", what, key);
	pad_last = pad_fn ? pad_fn() : 0;
	for (;;)
	{
		unsigned p = pressed();
		int x;

		if (app_events())
			return 0;
		frame++;
		backdrop();
		top_bar("DRIVE CHECK");
		text_centre(F_BOLD, 320, 118, 0, INK, what);
		text_centre(F_BODY, 320, 168, 0, DIM, "Prism has not sent the drive this command on this console yet.");
		snprintf(src_ln, sizeof src_ln, "It is the one %s sends. If the console stops answering", step_src);
		text_centre(F_BODY, 320, 194, 0, DIM, src_ln);
		text_centre(F_BODY, 320, 220, 0, DIM, "for a minute, hold the power button until it turns off.");
		text_centre(F_SMALL, 320, 262, 0, AMBER, "Each new command asks once. The log keeps where it got to.");
		legend_line();
		x = legend_item(LEFT, 0, NULL, "Send it");
		legend_item(x, 3, NULL, step_no);
		gfx_present();
		if (p & PAD_CIRCLE)
		{
			logf_("  the player said: %s", step_no);
			return 0;
		}
		if (p & PAD_CROSS)
		{
			logf_("  the player said send it");
			progress(NULL, "DRIVE CHECK", what, 0.5f, "Sent. If this stays for a minute, the console has stopped.");
			return 1;
		}
	}
}

static u32 img_fd, img_cd_calls, img_rd_calls, img_err_lines;
static int img_exf;
static rc_parts img_parts;
static u64 img_sectors0, img_log_at;
static u8 *img_raw;		/* one raw sector, for the title ID */

static int cb_read_cd(void *ctx, uint32_t lba, int audio, uint8_t *buf)
{
	s32 r = drv_read_cd(img_fd, lba, audio, buf, img_cd_calls < 3);

	(void)ctx;
	img_cd_calls++;
	if (r && img_err_lines < 40)
	{
		img_err_lines++;
		logf_("  READ CD %u (%s): result %x", (unsigned)lba, audio ? "audio" : "data", (u32)r);
	}
	return r != 0;
}

static u32 img_fast_calls;
static int img_sped;	/* v0.47: the drive was asked for its full speed: put back at the end */

static int cb_read_cd_n(void *ctx, uint32_t lba, uint32_t n, uint8_t *buf)
{
	s32 r = drv_read_cd_n(img_fd, lba, n, buf, img_fast_calls < 3);

	(void)ctx;
	img_fast_calls++;
	if (r && img_err_lines < 40)
	{
		img_err_lines++;
		logf_("  fast READ CD %u + %u: result %x", (unsigned)lba, (unsigned)n, (u32)r);
	}
	return r != 0;
}

/* v0.44: the fast read's first runs on a console, one at a time, each asked for first (Bananafuzzbuddy's rule),
 * each checked against the slow read that is known to work here. b holds DRV_FAST_MAX * DRV_RAWQ
 * bytes. Sets *fast (data sectors may come the fast way) and *fast_audio (music too); when a step
 * is turned down or does not work, the copy goes on the slow way.
 *   1. one sector (16) the fast way: must be byte for byte what the slow read gives;
 *   2. DRV_FAST_MAX sectors at once, from 16: each must be the data sector that belongs there;
 *   3. eight sectors of music at once: must be byte for byte what the slow music read gives. */
static void fast_checks(const rc_toc *toc, u8 *b, int *fast, int *fast_audio)
{
	const u32 data_end = toc->n > 1 ? toc->t[1].lba : toc->leadout;
	u32 k, okn = 0;
	s32 sr;

	*fast = *fast_audio = 0;
	if (step_known(step_no_key("readcd-fast")))
	{
		logf_("the fast read did not work with this build before: the slow way");
		return;
	}
	if (data_end < 16 + DRV_FAST_MAX + 400)
	{
		logf_("the data track is too short for the fast read's check: the slow way");
		return;
	}
	/* v0.47: no questions here any more (Bananafuzzbuddy, 11-23; all three ran on the CECHE01 that day).
	 * The checks themselves run on every copy. */
	sr = drv_read_cd(img_fd, 16, 0, img_raw, 0);
	if (sr)
		goto out;
	memset(b, 0xEE, DRV_RAWQ);
	sr = drv_read_cd_n(img_fd, 16, 1, b, 1);
	if (sr || memcmp(b, img_raw, RC_RAW))
	{
		logf_("  one sector the fast way: result %x, %s the slow read", (u32)sr, sr ? "not compared with" : "DIFFERENT from");
		step_note(step_no_key("readcd-fast"), "one sector did not come right");
		goto out;
	}
	logf_("  one sector the fast way: the same 2352 bytes as the slow read");
	step_mark("readcd-fast1");

	memset(b, 0xEE, DRV_FAST_MAX * DRV_RAWQ);
	sr = drv_read_cd_n(img_fd, 16, DRV_FAST_MAX, b, 1);
	for (k = 0; !sr && k < DRV_FAST_MAX; k++)
		okn += rc_data_ok(b + k * DRV_RAWQ, 16 + k);
	logf_("  %d sectors with one command: result %x, %u are the sectors that belong there; sector 16 %s", DRV_FAST_MAX, (u32)sr,
		(unsigned)okn, !sr && !memcmp(b, img_raw, RC_RAW) ? "the same as the slow read" : "DIFFERENT");
	if (sr || okn != DRV_FAST_MAX || memcmp(b, img_raw, RC_RAW))
	{
		step_note(step_no_key("readcd-fast"), "many sectors did not come right");
		goto out;
	}
	step_mark("readcd-fast");
	*fast = 1;

	if (toc->n > 1 && !step_known(step_no_key("readcd-fast-audio")))
	{
		const u32 t_end = toc->n > 2 ? toc->t[2].lba : toc->leadout, len = t_end - toc->t[1].lba;
		const u32 at = toc->t[1].lba + (len / 2 < 3000 ? len / 2 : 3000);
		u32 same = 0, quiet = 0, j;

		if (len < 800)
		{
			logf_("the first music track is too short for the fast read's check: music the slow way");
			goto out;
		}
		memset(b, 0xEE, 8 * DRV_RAWQ);
		sr = drv_read_cd_n(img_fd, at, 8, b, 1);
		for (k = 0; !sr && k < 8; k++)
		{
			if (drv_read_cd(img_fd, at + k, 1, img_raw, 0))
				break;
			same += !memcmp(b + k * DRV_RAWQ, img_raw, RC_RAW);
			for (j = 0; j < RC_RAW && !img_raw[j]; j++)
				;
			quiet += j == RC_RAW;
		}
		logf_("  music the fast way, 8 sectors at %u: result %x, %u the same as the slow read (%u of them silence)", (unsigned)at,
			(u32)sr, (unsigned)same, (unsigned)quiet);
		if (sr || same != 8)
		{
			step_note(step_no_key("readcd-fast-audio"), "music did not come the same");
			goto out;
		}
		step_mark("readcd-fast-audio");
		*fast_audio = 1;
	}
	else if (toc->n > 1)
		logf_("music the fast way did not come the same with this build before: music the slow way");
out:
	logf_("the copy will read %s", !*fast ? "one sector a command (the slow way)" : *fast_audio || toc->n == 1 ?
		"many sectors a command (the fast way)" : "the game data the fast way and the music the slow way");
}

static int cb_read_2048(void *ctx, uint32_t lba, uint32_t n, uint8_t *buf)
{
	u32 got = 0;
	s32 r = 0;
	int k;

	(void)ctx;
	if (img_rd_calls < 3)
		logf_("drive: next is a plain read (syscall 602) of %u sector(s) at %u", (unsigned)n, (unsigned)lba);
	for (k = 0; k < 100; k++)
	{
		r = st_read(img_fd, lba, n, buf, &got);
		if ((u32)r != DRV_EBUSY)
			break;
		usleep(10000);
	}
	if (img_rd_calls < 3 || ((r || got != n) && img_err_lines < 40))
	{
		if (r || got != n)
			img_err_lines++;
		logf_("  read %u + %u: result %x, %u sector(s), %d busy answer(s)", (unsigned)lba, (unsigned)n, (u32)r, (unsigned)got, k);
	}
	img_rd_calls++;
	return r != 0 || got != n;
}

static int cb_write(void *ctx, const uint8_t *buf, uint32_t len)
{
	(void)ctx;
	return img_exf ? exw_write(buf, len) : rc_parts_write(&img_parts, buf, len);
}

static int cb_tick(void *ctx, uint64_t d, uint64_t all)
{
	(void)ctx;
	(void)all;
	done = d;
	if (tb() - img_log_at > 5 * TB_HZ)
	{
		img_log_at = tb();
		logf_("  at %llu of %llu MB", (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
	}
	if (tb() - t_shown > TB_HZ / 2)
	{
		t_shown = tb();
		show(dest_name);
		if ((pressed() & PAD_CIRCLE) || app_events())
		{
			stop = 1;
			logf_("  stopped by the player");
			return 1;
		}
	}
	return 0;
}

static int cb_present(void *ctx)
{
	st_devinfo info;

	(void)ctx;
	memset(&info, 0, sizeof info);
	return st_info(DRV_DEV, &info) == 0 && info.total_sectors == img_sectors0;
}

static void cb_log(void *ctx, const char *line)
{
	(void)ctx;
	logf_("  %s", line);
}

/* one 2048-byte sector of user data, for the title ID */
static int rd_dvd(void *ctx, uint32_t lba, uint8_t *buf)
{
	if (cb_read_2048(ctx, lba, 1, img_raw))
		return -1;
	memcpy(buf, img_raw, 2048);
	return 0;
}

static int rd_cd(void *ctx, uint32_t lba, uint8_t *buf)
{
	int off;

	if (cb_read_cd(ctx, lba, 0, img_raw))
		return -1;
	off = rc_user_off(img_raw);
	if (off < 0)
		return -1;
	memcpy(buf, img_raw + off, 2048);
	return 0;
}

/* v0.42: getting the console to let Prism read a PS1 / PS2 disc. What 11-23 showed on Bananafuzzbuddy's
 * console (re_notes/ps12_disc_rip_2026-11-23.md, runs 2-7):
 *   - the drive reset (syscall 864, 0x5004 / 0x29) is refused (0x80010505) and is never sent now;
 *   - a disc put in on the XMB before Prism starts is mounted (/dev_bdvd) and still refused;
 *   - a disc put in while Prism runs is not even mounted: the system reports it as type 0;
 *   - BOTH copies that worked followed the same steps: webMAN unmounted a game image, then the
 *     disc was taken out and put back while Prism ran. The system then reported "PS2 disc" (type
 *     2), mounted it, and every plain read worked.
 * So the stages below, each tried only if the one before left the disc unreadable, and each logged:
 *   0. as the disc stands;
 *   1. webMAN's unmount with nothing mounted, then the disc put in again;
 *   2. a game image mounted through webMAN and unmounted again (the proven steps), then the disc
 *      put in again.
 * The log says which stage was needed. */
extern volatile int disc_in_events;	/* main.c */
static u8 probe_buf[2352] __attribute__((aligned(128)));
static int readcd_asked;

/* 0 = sector 16 came back: a DVD through the plain read, a CD through READ CD */
static int probe_read(int cd)
{
	u32 fd = 0, got = 0;
	s32 r, r2;

	r = st_open(DRV_DEV, &fd);
	if (r)
	{
		logf_("  probe: drive open %x", (u32)r);
		return -1;
	}
	if (cd)
	{
		if (!readcd_asked && !drive_step("readcd", "Read one sector of the disc"))
		{
			st_close(fd);
			return -2;
		}
		readcd_asked = 1;
		r2 = drv_read_cd(fd, 16, 0, probe_buf, 1);
		got = r2 == 0;
	}
	else
	{
		logf_("drive: next is a plain read (syscall 602) of sector 16");
		r2 = st_read(fd, 16, 1, probe_buf, &got);
		logf_("  plain read 16: result %x, %u sector(s)", (u32)r2, (unsigned)got);
	}
	st_close(fd);
	return r2 == 0 && got == 1 ? 0 : -1;
}

/* 1 = the disc is back and the console has mounted it; 0 = it came back but was not mounted;
 * -1 = the player stopped, or nothing came */
static int reinsert_prompt(void)
{
	u64 until = tb() + 180 * TB_HZ, t_back = 0;
	int in0 = disc_in_events, back = 0;

	logf_("  asking for the disc to be taken out and put back in");
	pad_last = pad_fn ? pad_fn() : 0;
	while (tb() < until)
	{
		unsigned p = pressed();

		if (app_events() || (p & PAD_CIRCLE))
		{
			logf_("  stopped by the player");
			return -1;
		}
		frame++;
		backdrop();
		top_bar("COPY DISC");
		if (!back)
		{
			text_centre(F_BOLD, 320, 150, 0, INK, "Take the disc out and put it back in");
			text_centre(F_BODY, 320, 200, 0, DIM, "The console lets Prism read a PS1 or PS2 disc only when the");
			text_centre(F_BODY, 320, 226, 0, DIM, "disc goes in after Prism has asked. Prism goes on by itself.");
		}
		else
		{
			text_centre(F_BOLD, 320, 150, 0, INK, "The disc is back");
			text_centre(F_BODY, 320, 200, 0, DIM, "Waiting for the console to take it in");
		}
		legend_line();
		legend_item(LEFT, 3, NULL, "Stop");
		gfx_present();
		if (!back && disc_in_events != in0 && disc_ps_type())
		{
			back = 1;
			t_back = tb();
			logf_("  a disc went in (type %x)", disc_ps_type());
		}
		if (back && is_dir(BD))
		{
			progress(NULL, "ONE MOMENT", "Looking at the disc", 0.1f, "/dev_bdvd");
			sleep(3);
			logf_("  the console mounted it after %u ms", ms(tb() - t_back));
			return 1;
		}
		if (back && tb() - t_back > 25 * TB_HZ)
		{
			logf_("  the console did not mount it in 25 s");
			return 0;
		}
	}
	return -1;
}

/* 1 = readable; 0 = not; -1 = the player stopped */
static int unlock_disc(int cd)
{
	char resp[256], enc[640], url[700];
	const char *helper = NULL;
	int k, r;

	readcd_asked = 0;
	logf_("unlock 0: the disc as it stands (/dev_bdvd %s)", is_dir(BD) ? "mounted" : "not mounted");
	r = probe_read(cd);
	if (r == 0)
		return 1;
	if (r == -2)
		return -1;

	logf_("unlock 1: webMAN unmount with nothing mounted, then the disc again");
	progress(NULL, "ONE MOMENT", title, 0.1f, "Asking the console for the disc");
	k = wm_get("/mount_ps3/unmount", resp, sizeof resp);
	logf_("  webMAN unmount: %d", k);
	sleep(2);
	r = reinsert_prompt();
	if (r < 0)
		return -1;
	if (r == 1 && probe_read(cd) == 0)
		return 1;

	/* a game image webMAN can mount: a PS1 game from Prism's own list (the proven case was one) */
	for (k = 0; k < n_games[P_PS1] && !helper; k++)
		if (games[P_PS1][k].kind < 4 && games[P_PS1][k].where != WHERE_DISC)
			helper = games[P_PS1][k].path;
	for (k = 0; k < n_games[P_PS3] && !helper; k++)
		if (games[P_PS3][k].kind < 4 && games[P_PS3][k].where != WHERE_DISC)
			helper = games[P_PS3][k].path;
	if (!helper)
	{
		logf_("unlock 2: no game image to mount (no PS1 or PS3 game in the lists)");
		return 0;
	}
	logf_("unlock 2: mount %s through webMAN, unmount it, then the disc again", helper);
	progress(NULL, "ONE MOMENT", title, 0.1f, "Asking the console for the disc");
	wm_urlenc(enc, sizeof enc, helper);
	snprintf(url, sizeof url, "/mount_ps3%s", enc);
	k = wm_get(url, resp, sizeof resp);
	logf_("  webMAN mount: %d", k);
	for (k = 0; k < 30 && !wm_mounted(NULL, 0); k++)
		usleep(500000);
	logf_("  mounted: %s after %d ms", wm_mounted(NULL, 0) ? "yes" : "NO", k * 500);
	sleep(3);
	k = wm_get("/mount_ps3/unmount", resp, sizeof resp);
	logf_("  webMAN unmount: %d", k);
	for (k = 0; k < 30 && wm_mounted(NULL, 0); k++)
		usleep(500000);
	sleep(2);
	r = reinsert_prompt();
	if (r < 0)
		return -1;
	return r == 1 && probe_read(cd) == 0;
}

static int rip_ps_image(int plat, unsigned type, char *msg, int msg_n)
{
	static u8 tocb[DRV_TOC_MAX + 60] __attribute__((aligned(64)));
	const int cd = type == DT_PSX_CD_ || type == DT_PS2_CD_;
	const char *pname = plat == P_PS2 ? "PS2" : "PS1", *art_title = NULL;
	char dst[300], base[120], id[16], why[160];
	st_devinfo info;
	rc_toc toc;
	rc_stats st;
	rc_io io;
	exw_dev dv;
	u8 *b = NULL;
	u32 sectors = 0;
	int r, d = -1, k, opened = 0, ps, fast = 0, fast_audio = 0;
	s32 sr;

	id[0] = 0;
	img_fd = 0;
	img_exf = 0;
	img_cd_calls = img_rd_calls = img_err_lines = 0;
	img_sped = 0;
	memset(&toc, 0, sizeof toc);
	memset(&st, 0, sizeof st);
	drv_set_log(logf_);
	snprintf(title, sizeof title, "%s disc", pname);
	snprintf(tid, sizeof tid, "-");
	logf_("a %s %s in the drive (disc type %x)", pname, cd ? "CD" : "DVD", type);
	if (!type)
	{
		/* CD or DVD decides which read commands are sent: never guess it */
		snprintf(msg, msg_n, "The console no longer says what kind of disc this is. Take it out, put it in again and try once more.");
		return -1;
	}

	k = unlock_disc(cd);
	if (k <= 0)
	{
		snprintf(msg, msg_n, k < 0 ? "Nothing copied" : "The console would not let Prism read this disc: nothing copied (the log has the steps tried)");
		return -1;
	}
	type = disc_ps_type();
	logf_("state: /dev_bdvd %s, /dev_bdvd/SYSTEM.CNF %s, disc type %x", is_dir(BD) ? "mounted" : "not mounted",
		self_magic(BD "/SYSTEM.CNF") >= 0 ? "readable" : "not there", type);
	if (!type || (type == DT_PSX_CD_ || type == DT_PS2_CD_) != cd)
	{
		snprintf(msg, msg_n, "That is a different kind of disc now: nothing copied. Try again.");
		return -1;
	}
	{
		/* v0.44: never read through a game image Cobra shows in place of the drive */
		unsigned real = 0, eff = 0;

		if (disc_emulated(&real, &eff))
		{
			logf_("Cobra shows a game image in place of the drive (real %x, shown %x): not read", real, eff);
			snprintf(msg, msg_n, "A game image is mounted over the drive: nothing copied. Try again.");
			return -1;
		}
	}
	progress(NULL, "ONE MOMENT", title, 0.1f, "Asking the drive");

	/* 1. the drive answers with the disc's size (IRISMAN get_disc_ready: until it is not busy) */
	memset(&info, 0, sizeof info);
	for (k = 0; k < 200; k++)
	{
		sr = st_info(DRV_DEV, &info);
		if (sr == 0 && info.total_sectors && info.total_sectors < 0x7FFFFFFFull)
			break;
		if (k % 10 == 0)
			progress(NULL, "ONE MOMENT", title, 0.15f, "Waiting for the drive");
		usleep(100000);
	}
	logf_("drive info: result %x, %llu sectors of %u, after %d ms", (u32)sr, (unsigned long long)info.total_sectors,
		info.sector_size, k * 100);
	if (sr || !info.total_sectors || info.total_sectors >= 0x7FFFFFFFull)
	{
		snprintf(msg, msg_n, "The drive did not come back with the disc (%x). Take the disc out, put it in again and try once more.", (u32)sr);
		return -1;
	}
	img_sectors0 = info.total_sectors;
	sr = st_open(DRV_DEV, &img_fd);
	logf_("drive open: result %x", (u32)sr);
	if (sr)
	{
		snprintf(msg, msg_n, "Could not open the disc drive (%x)", (u32)sr);
		return -1;
	}
	opened = 1;
	/* 256 DVD sectors, or DRV_FAST_MAX CD sectors with their sub-channel bytes; then one raw sector */
	b = (u8 *)memalign(128, DRV_FAST_MAX * DRV_RAWQ + RC_RAW);
	if (!b)
	{
		snprintf(msg, msg_n, "Out of memory");
		r = -1;
		goto finish;
	}
	img_raw = b + DRV_FAST_MAX * DRV_RAWQ;
	r = -1;

	if (cd)
	{
		int len = 0;

		/* 3. the track list */
		if (!drive_step("toc", "Read the disc's track list"))
		{
			snprintf(msg, msg_n, "Nothing copied");
			goto finish;
		}
		sr = drv_toc(img_fd, tocb, &len);
		if (sr)
		{
			snprintf(msg, msg_n, "The drive would not give the disc's track list (%x): nothing copied", (u32)sr);
			goto finish;
		}
		step_mark("toc");
		if (rc_toc_parse(tocb, len, &toc, why, sizeof why))
		{
			logf_("  track list refused: %s", why);
			snprintf(msg, msg_n, "Not copied: %s", why);
			goto finish;
		}
		for (k = 0; k < toc.n; k++)
			logf_("  track %d: %s, starts at sector %u", k + 1, toc.t[k].audio ? "audio" : "data", (unsigned)toc.t[k].lba);
		logf_("  the disc ends at sector %u", (unsigned)toc.leadout);

		/* 4. one data sector, then (if there is music) one audio sector */
		sr = drv_read_cd(img_fd, 16, 0, img_raw, 1);
		if (sr || rc_user_off(img_raw) < 0)
		{
			snprintf(msg, msg_n, sr ? "The drive would not read the disc (%x): nothing copied" :
				"The disc's sector 16 is not a data sector (%x): nothing copied", (u32)sr);
			goto finish;
		}
		step_mark("readcd");
		if (toc.n > 1)
		{
			if (!drive_step("readcd-audio", "Read one sector of the disc's music"))
			{
				snprintf(msg, msg_n, "Nothing copied");
				goto finish;
			}
			sr = drv_read_cd(img_fd, toc.t[1].lba, 1, img_raw, 1);
			if (sr)
			{
				snprintf(msg, msg_n, "The drive would not read the disc's music (%x): nothing copied", (u32)sr);
				goto finish;
			}
			step_mark("readcd-audio");
		}
		/* 4b. v0.44: the fast read, if it proves itself on this disc (never a reason to stop).
		 * v0.46: OFF. It worked on Bananafuzzbuddy's console at the first try (11-23 21:08: all three checks
		 * passed, 19456 sectors in 76 commands, none failed, none wrong) and was no faster:
		 * 0.63 MB/s against 0.70 one sector at a time. The drive turns a CD at about 4x and the
		 * number of commands does not matter. It cost three questions on a first copy (Bananafuzzbuddy:
		 * "can we remove ... it saying irisman every single time") for nothing, so it is left out;
		 * the code stays for a console where it might matter. */
		/* v0.47: first ask the drive for its full speed (drive.c drv_set_cd_speed). The one new
		 * command, so it asks once per console; Circle, a refusal, or a sector 16 that no longer
		 * reads right afterwards, and the copy runs at the drive's own speed as before. */
		if (!step_known(step_no_key("cdspeed")))
		{
			int go;

			step_src = "Linux on a PS3";
			step_no = "Slow way";
			go = drive_step("cdspeed", "Ask the drive to read faster");
			step_src = "multiMAN";
			step_no = "Stop";
			if (go)
			{
				sr = drv_set_cd_speed(img_fd, DRV_SPEED_MAX);
				if (sr == 0 && (drv_read_cd(img_fd, 16, 0, img_raw, 1) || !rc_data_ok(img_raw, 16)))
				{
					logf_("  sector 16 does not read right at that speed: back to 4x");
					drv_set_cd_speed(img_fd, DRV_SPEED_4X);
					sr = -1;
				}
				if (sr)
					step_note(step_no_key("cdspeed"), "the drive would not take it");
				else
				{
					step_mark("cdspeed");
					img_sped = 1;
				}
			}
		}
		else
			logf_("the drive's speed command did not work with this build before: not sent");
		if (FAST_CD)
			fast_checks(&toc, b, &fast, &fast_audio);
		progress(NULL, "ONE MOMENT", title, 0.2f, "Asking the drive");
		ps = rc_disc_id(rd_cd, NULL, id, sizeof id);
		total = (u64)toc.leadout * RC_RAW;
	}
	else
	{
		/* 3. a plain read: the call that failed on every PS2 disc before the reset was there */
		if (info.sector_size != 2048)
		{
			snprintf(msg, msg_n, "The drive reports %u-byte sectors for this disc: not copied", info.sector_size);
			goto finish;
		}
		if (cb_read_2048(NULL, 16, 1, img_raw))
		{
			snprintf(msg, msg_n, "The console would not let Prism read this disc: nothing copied (the log has the code)");
			goto finish;
		}
		step_mark("read");
		ps = rc_disc_id(rd_dvd, NULL, id, sizeof id);
		sectors = (u32)info.total_sectors;
		total = (u64)sectors * 2048;
	}
	logf_("title ID \"%s\" (SYSTEM.CNF says %s); the image is %llu bytes", id, ps == 1 ? "PS2" : ps == 0 ? "PS1" : "nothing", (unsigned long long)total);
	if ((ps == 0 && plat == P_PS2) || (ps == 1 && plat == P_PS1))
	{
		/* v0.44: what was read is not the disc the console reports (11-23: a mounted PS1 image
		 * read as "the PS2 DVD in the drive"): a second guard, from the disc's own files */
		logf_("  the disc's files say %s, the console says a %s disc: not copied", ps ? "PS2" : "PS1", pname);
		snprintf(msg, msg_n, "The console says a %s disc is in the drive, but what Prism read is a %s game: nothing copied. Take the disc out, put it in again and try once more.",
			pname, ps ? "PS2" : "PS1");
		goto finish;
	}
	if (id[0])
	{
		static char listed[100];

		art_find(id, &art_title);
		if (!(art_title && art_title[0]) && title_lookup(id, listed, sizeof listed) == 0)
			art_title = listed;	/* v0.43: no cover entry: the title list (titles.c) */
		snprintf(tid, sizeof tid, "%s", id);
	}
	if (art_title && art_title[0])
		snprintf(title, sizeof title, "%s", art_title);
	else if (id[0])
		snprintf(title, sizeof title, "%s disc %s", pname, id);
	rc_name(base, 100, art_title, id, plat == P_PS2 ? "PS2 disc" : "PS1 disc");

	/* 5. where to */
	snprintf(dest_sub, sizeof dest_sub, plat == P_PS2 ? "PS2ISO" : "PSXISO");
	snprintf(dest_name, sizeof dest_name, "%s%s", base, cd ? ".bin" : ".iso");
	pick_head = "COPY DISC";
	src_dev = -1;
	bigfile = total > FAT32_MAX;
	ps2_img = plat == P_PS2;
	progress(NULL, "ONE MOMENT", title, 0.3f, "Looking at the storage devices");
	scan_devs();
	d = choose();
	if (d < 0)
	{
		snprintf(msg, msg_n, "Nothing copied");
		logf_("backed out");
		r = 1;
		goto finish;
	}
	if (d == EXF)
	{
		if (exc_open(&dv, why, sizeof why) || exw_begin(&dv, dest_sub, dest_name, total, why, sizeof why))
		{
			exc_close();
			snprintf(msg, msg_n, "Not copied: %s", why);
			goto finish;
		}
		img_exf = 1;
		logf_("image to the card's exFAT part: %s/%s", dest_sub, dest_name);
	}
	else
	{
		snprintf(dst, sizeof dst, "%s/%s", devs[d].path, sub_on(d));
		sysFsMkdir(dst, 0777);
		pk_begin(&img_parts, dst, dest_name);
		if (rc_parts_begin(&img_parts, total, RC_PART_MAX, devs[d].fat ? RC_WHOLE_MAX : 0))
		{
			snprintf(msg, msg_n, "Not copied: the image is too big for %d parts", RC_PARTS_MAX);
			goto finish;
		}
		logf_("image to %s/%s%s", dst, dest_name, img_parts.split ? " in parts under 4 GiB" : "");
	}

	/* 6. the copy */
	io.read_cd = cb_read_cd;
	io.read_2048 = cb_read_2048;
	io.write = cb_write;
	io.tick = cb_tick;
	io.present = cb_present;
	io.log = cb_log;
	io.ctx = NULL;
	io.read_cd_n = fast ? cb_read_cd_n : NULL;
	io.fast_audio = fast_audio;
	img_cd_calls = img_rd_calls = img_fast_calls = 0;
	done = 0;
	t0 = img_log_at = t_shown = tb();
	show("starting");
	pad_last = pad_fn ? pad_fn() : 0;
	r = cd ? rc_rip_cd(&toc, &io, b, fast ? DRV_FAST_MAX : 64, &st) : rc_rip_dvd(sectors, &io, b, 256, &st);
	logf_("image ended %d: %llu of %llu bytes in %.1f s; unreadable %u (+%u in the gaps), first %u; second tries %u", r,
		(unsigned long long)st.done, (unsigned long long)total, (double)(tb() - t0) / TB_HZ, (unsigned)st.bad,
		(unsigned)st.gap_bad, (unsigned)st.first_bad, (unsigned)st.retries);
	if (cd)
		logf_("  the fast read gave %u sectors in %u command(s); %u command(s) failed, %u data sector(s) came wrong, given up at %u; one-sector reads %u",
			(unsigned)st.fast, (unsigned)img_fast_calls, (unsigned)st.fast_fail, (unsigned)st.fast_wrong, (unsigned)st.fast_off_at,
			(unsigned)img_cd_calls);

	/* 7. keep it whole, or keep nothing */
	if (img_exf)
	{
		if (!r && exw_finish(why, sizeof why))
		{
			logf_("  exFAT: %s", why);
			r = -7;
		}
		else if (r)
			exw_abort();
		exc_close();
		lr_fs_forget();
	}
	else
	{
		if (rc_parts_end(&img_parts) && !r)
			r = -5;
		if (pk_finish(&img_parts, r == 0) && !r)
			r = -6;
		if (d == 0)
			lr_fs_forget();		/* the card reader's cached sectors may hold the folder as it was */
		if (!r && cd)
		{
			char cue[4096], cp[440], bn[140];
			int n;
			FILE *f;

			snprintf(bn, sizeof bn, "%s.bin", base);
			snprintf(cp, sizeof cp, "%s/%s.cue", dst, base);
			n = rc_cue(&toc, bn, cue, sizeof cue);
			f = n > 0 ? fopen(cp, "wb") : NULL;
			if (f)
			{
				fwrite(cue, 1, (size_t)n, f);
				fclose(f);
				sysFsChmod(cp, 0777);
			}
			logf_("  cue sheet %s: %s", cp, f ? "written" : "NOT written");
		}
	}
	if (r == RC_STOP)
		snprintf(msg, msg_n, "Stopped: nothing kept");
	else if (r == RC_GONE)
		snprintf(msg, msg_n, "The disc left the drive: nothing kept");
	else if (r == RC_EREAD)
		snprintf(msg, msg_n, "The disc could not be read (from sector %u): nothing kept", (unsigned)st.first_bad);
	else if (r == RC_EWRITE)
		snprintf(msg, msg_n, "Could not write to %s at %llu MB: nothing kept", devs[d].name, (unsigned long long)(st.done >> 20));
	else if (r)
		snprintf(msg, msg_n, "The copy could not be finished on %s (%d): nothing kept", devs[d].name, r);
	else
	{
		double secs = (double)(tb() - t0) / TB_HZ;

		if (st.bad)
			snprintf(msg, msg_n, "%s copied to %s, but %u sector(s) could not be read and are blank: it may not play right", title,
				devs[d].name, (unsigned)st.bad);
		else
			snprintf(msg, msg_n, "%s copied to %s: %.0f MB in %d:%02d", title, devs[d].name, total / 1e6, (int)secs / 60, (int)secs % 60);
	}
finish:
	ps2_img = 0;
	free(b);
	img_raw = NULL;
	if (opened)
	{
		if (img_sped)
		{
			/* the speed it read at before, for whatever plays the disc next */
			drv_set_cd_speed(img_fd, DRV_SPEED_4X);
			img_sped = 0;
		}
		sr = st_close(img_fd);
		logf_("drive closed: result %x", (u32)sr);
	}
	return r;
}
