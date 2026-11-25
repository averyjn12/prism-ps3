/* Copy the PS3 game disc in the drive onto a storage device the player picks (SD card, internal
 * hard disk, USB drives, Memory Stick / CF slots), as a game folder <device>/GAMES/<TITLE_ID>,
 * from inside the app (2026-11-22; picker + confirmation v0.13).
 *
 * Why inside the app: webMAN serves /dev_bdvd from the XMB process, and there the disc reads
 * ENCRYPTED - the FTP rip of Borderlands had an EBOOT.BIN of random bytes and would not start
 * (80010017). Inside this app (a running game) the same EBOOT.BIN reads "SCE\0" (v0.12, owner's
 * console, 2026-11-22 22:31). The copy still REFUSES to start unless the disc's EBOOT.BIN begins
 * with the SELF magic, and checks the copy's EBOOT.BIN the same way at the end.
 *
 * v0.12 froze the console about two minutes in, after 14 files at ~0.1 MB/s, and its log was
 * written only at the end. Now every step goes to /dev_hdd0/tmp/prism_rip.log AS IT HAPPENS (each
 * line opened, written, closed), with read and write times per file, so a stall or a freeze shows
 * where it was. Reads in 1 MB pieces (v0.12: 4 MB). Circle stops the copy between pieces.
 *
 * A game mounted by webMAN sits over /dev_bdvd: v0.13 does not unmount it from inside the app
 * (v0.12 did, the suspect for the freeze until the log says otherwise); it asks webMAN whether
 * something is mounted and, if so, says so and copies nothing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <lv2/sysfs.h>
#include <sys/file.h>
#include "gfx.h"
#include "ui.h"
#include "look.h"
#include "games.h"
#include "screens.h"
#include "webman.h"
#include "rip.h"
#include "sfx.h"
#include "launch.h"	/* v0.72: pps_is_link */
#include "exfat_card.h"
#include "lr.h"
#include "manage.h"
#include "dirlist.h"
#include "version.h"
#include "ripcore.h"
#include "storage.h"
#include "drive.h"
#include "cardcheck.h"	/* v0.65 */

#define BD		"/dev_bdvd"
#define RIP_LOG		"/dev_hdd0/tmp/prism_rip.log"
#define BUF_SZ		(1u << 20)
#define FAT32_MAX	0xFFFFFFFFull
#define TB_HZ		79800000ULL

typedef struct
{
	const char *path, *name;
	int fat;		/* FAT32: no file of 4 GiB or more */
	int present;
	u64 free;
} dev_t_;

static dev_t_ devs[] = {
	{ "/dev_sd", "SD card", 1, 0, 0 },
	{ "/dev_hdd0", "Internal hard disk", 0, 0, 0 },
	{ "/dev_usb000", "USB 0", 1, 0, 0 }, { "/dev_usb001", "USB 1", 1, 0, 0 },
	{ "/dev_usb002", "USB 2", 1, 0, 0 }, { "/dev_usb003", "USB 3", 1, 0, 0 },
	{ "/dev_usb004", "USB 4", 1, 0, 0 }, { "/dev_usb005", "USB 5", 1, 0, 0 },
	{ "/dev_usb006", "USB 6", 1, 0, 0 }, { "/dev_usb007", "USB 7", 1, 0, 0 },
	{ "/dev_ms", "Memory Stick", 1, 0, 0 }, { "/dev_cf", "CompactFlash", 1, 0, 0 },
	{ "exfat:", "SD card, exFAT part", 0, 0, 0 },	/* v0.37: through exfat_w.c, PS2 games only */
};
#define NDEV	((int)(sizeof devs / sizeof devs[0]))
#define EXF	(NDEV - 1)
static int exf_ok;		/* v0.37: the exFAT part can take this copy (exw_begin said yes) */
static char exf_why[160];

static u64 total, done, t0, t_rd, t_wr;
static int nfiles, ndirs, nfile_at, bigfile, stop;
static char title[100], tid[16], first_bad[300];
static u8 *buf;
static unsigned (*pad_fn)(void);
static u64 t_shown;
int app_events(void);	/* main.c */
static unsigned pad_last;
/* v0.15: the same picker copies a game from one device to another (copy_game) */
static const char *pick_head = "COPY DISC";
static char dest_sub[16] = "GAMES", dest_name[128];
static int src_dev = -1;
/* v0.38: the thing being placed is a PS2 image. It goes where Prism plays PS2 games from: the hard
 * disk, the card's exFAT part, or the card's FAT32 part - there in /PS2PRISM (webMAN never lists that
 * folder), as parts under 4 GiB when it is too big for one FAT32 file (the card reader joins
 * "<name>.0", "<name>.1" ...; Bananafuzzbuddy's idea, 11-23; MGS2 already plays that way from the card) */
static int ps2_img;
#define PS2_FAT_DIR	"PS2PRISM"
/* v0.67: several games in one go (copy_games, at the end of this file). The device is chosen once, for
 * all of them; then copy_game() runs for each game as it does for one, only without asking again. */
static int batch_d = -1;	/* the device chosen for the batch; -1 = one game: ask */
static int batch_k, batch_n;	/* game k of n (1-based); n 0 = no batch */
static int pick_many;		/* the picker is choosing for several games */
static u64 exf_need;		/* the batch's largest single file: what the exFAT part must take in one run */
static int last_result;		/* of the last copy_game(): 1 copied, 2 it was already there, 0 not copied */
static int rip_ps_image(int plat, unsigned type, char *msg, int msg_n);

static u64 tb(void)
{
	u64 t;

	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

static unsigned ms(u64 ticks)
{
	return (unsigned)(ticks * 1000ULL / TB_HZ);
}

/* one line to the log, opened and closed each time so that it survives a freeze */
static void logf_(const char *fmt, ...)
{
	FILE *f = fopen(RIP_LOG, "a");
	va_list ap;

	if (!f)
		return;
	fprintf(f, "%7u ", ms(tb() - t0));
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static unsigned pressed(void)
{
	unsigned b = pad_fn ? pad_fn() : 0, p = b & ~pad_last;

	pad_last = b;
	if (p)	/* v0.72: the copy screens' own menus make the menu sounds too */
		sfx((p & PAD_CIRCLE) ? SFX_BACK : (p & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) ? SFX_MOVE : SFX_OK);
	return p;
}

static int is_dir(const char *p)
{
	sysFSStat st;

	return sysFsStat(p, &st) == 0 && (st.st_mode & 0170000) == 0040000;
}

/* sizes first: the whole tree, and any file FAT32 cannot hold */
static int measure(const char *dir, int depth)
{
	dl_ent *v;
	int k, m, r = 0;

	if (depth > 24 || (m = dir_list(dir, &v)) < 0)
		return -1;
	for (k = 0; k < m && !r; k++)
	{
		char p[1024];

		snprintf(p, sizeof p, "%s/%s", dir, v[k].name);
		if (v[k].dir)
		{
			ndirs++;
			r = measure(p, depth + 1);
		}
		else
		{
			total += v[k].size;
			nfiles++;
			if (v[k].size > FAT32_MAX && !bigfile)
			{
				bigfile = 1;
				snprintf(first_bad, sizeof first_bad, "%s", p);
			}
		}
	}
	free(v);
	return r;
}

static const char *show_head = "COPYING THE DISC";	/* v0.30: the PSP card copy says its own */

static void show(const char *what)
{
	char ln[200];
	double secs = (double)(tb() - t0) / TB_HZ, mbs = secs > 0.5 ? done / 1e6 / secs : 0.0;
	int eta = mbs > 0.05 ? (int)((total - done) / 1e6 / mbs) : 0;

	snprintf(ln, sizeof ln, "%s  " "\x7f" "  %.0f of %.0f MB  " "\x7f" "  %.1f MB/s  " "\x7f" "  %d:%02d left  " "\x7f" "  Circle stops",
		what, done / 1e6, total / 1e6, mbs, eta / 60, eta % 60);
	progress(NULL, show_head, title, total ? (float)((double)done / (double)total) : 0.0f, ln);
}

/* Straight lv2 calls, 1 MB at a time. v0.12/v0.13 used stdio, and PSL1GHT's newlib (BUFSIZ 1024)
 * cut every 1 MB fwrite into 1 KB writes: the SD slot pays ~8.6 ms per write call, so the copy
 * ran at 0.12 MB/s against webMAN's 3.6 MB/s into the same card (v0.13 log, 2026-11-22). */
static int link_hit;	/* v0.72: the last copy_file was refused because its target is a PPSSPP stand-in */

static int copy_file(const char *src, const char *dst, u64 size)
{
	s32 in, out;
	u64 got = 0, last = done, a, rd0 = 0, wr0 = 0;
	const char *b = strrchr(src, '/');

	nfile_at++;
	logf_("> %d/%d %llu %s", nfile_at, nfiles, (unsigned long long)size, src);
	a = tb();
	if (pps_is_link(dst))
	{
		/* v0.72: this name on the hard disk stands for a game on a card, for PPSSPP (launch.h), and while the
		 * console stays on it IS that game: opening it for writing would empty the game on the card */
		logf_("  refused: %s is PPSSPP's stand-in for a card game", dst);
		link_hit = 1;
		return -6;
	}
	if (sysFsOpen(src, SYS_O_RDONLY, &in, NULL, 0) != 0)
		return -1;
	if (sysFsOpen(dst, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &out, NULL, 0) != 0)
	{
		sysFsClose(in);
		return -2;
	}
	sysFsChmod(dst, 0777);
	logf_("  opened in %u ms", ms(tb() - a));
	for (;;)
	{
		u64 n = 0, w = 0;

		a = tb();
		if (sysFsRead(in, buf, BUF_SZ, &n) != 0)
		{
			sysFsClose(in);
			sysFsClose(out);
			return -6;
		}
		rd0 += tb() - a;
		if (n == 0)
			break;
		a = tb();
		if (sysFsWrite(out, buf, n, &w) != 0 || w != n)
		{
			sysFsClose(in);
			sysFsClose(out);
			return -3;
		}
		wr0 += tb() - a;
		got += n;
		done += n;
		if (done - last >= (4u << 20) || tb() - t_shown > TB_HZ / 2)
		{
			show(b ? b + 1 : src);
			t_shown = tb();
			last = done;
			if ((pressed() & PAD_CIRCLE) || app_events())
			{
				stop = 1;
				sysFsClose(in);
				sysFsClose(out);
				logf_("  stopped by the player");
				return -9;
			}
		}
		if (got % (64u << 20) == 0)
			logf_("  %llu MB, read %u ms, write %u ms", (unsigned long long)(got >> 20), ms(rd0), ms(wr0));
	}
	sysFsClose(in);
	a = tb();
	if (sysFsClose(out) != 0)
		return -4;
	if (tb() - t_shown > TB_HZ / 4)
	{
		show(b ? b + 1 : src);
		t_shown = tb();
		if ((pressed() & PAD_CIRCLE) || app_events())
			stop = 1;
	}
	wr0 += tb() - a;
	t_rd += rd0;
	t_wr += wr0;
	logf_("  done %llu bytes: read %u ms, write %u ms", (unsigned long long)got, ms(rd0), ms(wr0));
	return got == size ? 0 : -5;
}

static int copy_tree(const char *src, const char *dst, int depth)
{
	dl_ent *v;
	int k, m, r = 0;

	sysFsMkdir(dst, 0777);
	if (depth > 24 || (m = dir_list(src, &v)) < 0)
		return -10;
	for (k = 0; k < m && r == 0; k++)
	{
		char s[1024], d[1024];

		snprintf(s, sizeof s, "%s/%s", src, v[k].name);
		snprintf(d, sizeof d, "%s/%s", dst, v[k].name);
		if (v[k].dir)
			r = copy_tree(s, d, depth + 1);
		else
		{
			r = copy_file(s, d, v[k].size);
			if (!r && stop)
				r = -9;
			if (r && r != -9)
				snprintf(first_bad, sizeof first_bad, "%s (%d)", s, r);
		}
	}
	free(v);
	return r;
}

static int self_magic(const char *path)
{
	u8 m[4] = { 0 };
	FILE *f = fopen(path, "rb");

	if (!f)
		return -1;
	fread(m, 1, 4, f);
	fclose(f);
	return (m[0] == 'S' && m[1] == 'C' && m[2] == 'E' && m[3] == 0) ? 0 : 1;
}

static int sfo_key(const char *path, const char *key, char *out, int n)
{
	static u8 d[8192];
	FILE *f = fopen(path, "rb");
	int got;

	out[0] = 0;
	if (!f)
		return -1;
	got = (int)fread(d, 1, sizeof d, f);
	fclose(f);
	return sfo_get(d, got, key, out, n);
}

static void fmt_size(char *o, int n, u64 b)
{
	if (b >= (1ull << 30))
		snprintf(o, n, "%.1f GB", b / 1073741824.0);
	else
		snprintf(o, n, "%llu MB", (unsigned long long)(b >> 20));
}

/* ---- the choice: which device, then yes / no ---- */

/* v0.22: a PS3 game copied to the SD card does not start: Borderlands from the card's FAT32
 * part gave a black screen, while the same files ran from a USB stick and from the hard disk
 * (owner, 2026-11-23). So the card (devs[0]) is not offered for PS3 games. */
static int ps3_dest(void)
{
	return !strcmp(dest_sub, "GAMES") || !strcmp(dest_sub, "PS3ISO");
}

/* v0.48: the three card slots are one reader: none of them is offered for PS3 games */
static int slot_dev(int i)
{
	return i == 0 || i == 10 || i == 11;
}

static int usable(int i)
{
	if (slot_dev(i) && ps3_dest())
		return 0;
	if (i == EXF)
		return devs[i].present && exf_ok && (!pick_many || devs[i].free >= total);	/* v0.67: room for the whole batch */
	if (i == 0 && ps2_img)		/* in parts if need be */
		return devs[i].present && i != src_dev && devs[i].free >= total + (64u << 20);
	return devs[i].present && i != src_dev && devs[i].free >= total + (64u << 20) && !(devs[i].fat && bigfile);
}

/* the folder on device i that the copy goes into */
static const char *sub_on(int i)
{
	return i == 0 && ps2_img ? PS2_FAT_DIR : dest_sub;
}

static void draw_pick(int sel, int asking, int yes)
{
	char ln[200], a[32];
	int i, y = 120;

	backdrop();
	top_bar(pick_head);
	fmt_size(a, sizeof a, total);
	{
		char raw[200];

		snprintf(raw, sizeof raw, "%s  " "\x7f" "  %s  " "\x7f" "  %s", title, tid, a);
		fit(ln, sizeof ln, F_SMALL, raw, RIGHT - LEFT);
	}
	text_centre(F_SMALL, 320, 84, 1, DIM, ln);
	text_centre(F_BODY, 320, 100, 0, INK, pick_many ? "Copy them to which device?" : "Copy it to which device?");
	for (i = 0; i < NDEV; i++)
	{
		int ok = usable(i);

		if (!devs[i].present)
			continue;
		if (i == sel)
		{
			rect(LEFT, (float)y - 4.0f, RIGHT, (float)y + 22.0f, RGBA(0x7F, 0xE3, 0xFF, 0x1C));
			frame_rect(LEFT, (float)y - 4.0f, RIGHT, (float)y + 22.0f, 1.0f, GLOW);
		}
		text(F_BODY, LEFT + 12, y, ok ? (i == sel ? INK : DIM) : FAINT, devs[i].name);
		fmt_size(a, sizeof a, devs[i].free);
		if (i == src_dev)
			snprintf(ln, sizeof ln, pick_many ? "they are here" : "it is here");
		else if (slot_dev(i) && ps3_dest())
			snprintf(ln, sizeof ln, "PS3 games do not start from the card slot");
		else if (i == 0 && ps2_img && bigfile)
			snprintf(ln, sizeof ln, ok ? "%s free, as parts under 4 GB" : "%s free, not enough room", a);
		else if (i == EXF)
		{
			if (ok)
				snprintf(ln, sizeof ln, "%s free", a);
			else
				fit(ln, sizeof ln, F_SMALL, exf_why, 330);
		}
		else if (!devs[i].fat || !bigfile)
			snprintf(ln, sizeof ln, ok ? "%s free" : "%s free  " "\x7f" "  not enough room", a);
		else
			snprintf(ln, sizeof ln, "a file is over 4 GB: FAT32 cannot hold it");
		text_ex(F_SMALL, RIGHT - 12 - text_w(F_SMALL, ln, 1), y + 3, 1.0f, 1, ok ? DIM : FAINT, ln);
		y += 30;
	}
	legend_line();
	{
		int x = legend_item(LEFT, 0, NULL, "Choose");

		legend_item(x, 3, NULL, "Back");
	}
	if (asking)
	{
		rect(LX0, 0.0f, LX1, 448.0f, RGBA(0x02, 0x04, 0x0C, 0x58));
		rect(120.0f, 140.0f, 520.0f, 300.0f, RGBA(0x0D, 0x14, 0x33, 0x80));
		frame_rect(120.0f, 140.0f, 520.0f, 300.0f, 1.0f, LINE);
		spectrum_bar(120.0f, 520.0f, 140.0f, 2.0f, 6, 0x80);
		{
			char raw[200];

			snprintf(raw, sizeof raw, "Copy %s to %s?", title, devs[sel].name);
			fit(ln, sizeof ln, F_BOLD, raw, 380);
		}
		text_centre(F_BOLD, 320, 156, 0, INK, ln);
		{
			char raw[300];

			if (pick_many && sel == EXF)
				snprintf(raw, sizeof raw, "They go to the exFAT part's /%s, one after another", dest_sub);
			else if (pick_many)
				snprintf(raw, sizeof raw, "They go to %s/%s, one after another", devs[sel].path, sub_on(sel));
			else if (sel == 0 && ps2_img && bigfile)
				snprintf(raw, sizeof raw, "It goes to %s/%s/%s.0, .1 ...", devs[sel].path, sub_on(sel), dest_name);
			else if (sel == EXF)
				snprintf(raw, sizeof raw, "It goes to the exFAT part's /%s/%s", dest_sub, dest_name);
			else
				snprintf(raw, sizeof raw, "It goes to %s/%s/%s", devs[sel].path, sub_on(sel), dest_name);
			fit(ln, sizeof ln, F_SMALL, raw, 380);
		}
		text_centre(F_SMALL, 320, 192, 0, DIM, ln);
		text_centre(F_SMALL, 320, 212, 0, AMBER, pick_many ? "Anything already there with the same names is replaced." : "Anything already there with that name is replaced.");
		for (i = 0; i < 2; i++)
		{
			float bx = i ? 330.0f : 200.0f;

			if (yes == i)
			{
				rect(bx, 246.0f, bx + 110.0f, 276.0f, RGBA(0x7F, 0xE3, 0xFF, 0x1C));
				frame_rect(bx, 246.0f, bx + 110.0f, 276.0f, 1.0f, GLOW);
			}
			text_centre(F_BODY, (int)bx + 55, 252, 0, yes == i ? INK : DIM, i ? "Yes" : "No");
		}
	}
}

static void scan_devs(void)
{
	int i;

	for (i = 0; i < NDEV; i++)
	{
		u32 bs = 0;
		u64 fb = 0;

		if (i == EXF)
		{
			/* v0.37: the card's exFAT part: listed for PS2 copies, usable when the writer has
			 * checked the volume, the folder and the name (exw_begin) - nothing is written here */
			exw_dev dv;

			devs[i].present = 0;
			devs[i].free = 0;
			exf_ok = 0;
			exf_why[0] = 0;
			/* v0.65: never written unless the card was read from a slot that passed the check (cardcheck.h) */
			if (card_state || slot_bad(slot_of_dev(card_dev)))
			{
				logf_("exFAT part: not offered, %s", card_state == LR_E_BADSLOT ? "the card's slot gives damaged data" : "no card read");
				continue;
			}
			{
				/* v0.66: with cards in several slots, the first one read (games.c card_dev); say which */
				static char exf_name[48];

				snprintf(exf_name, sizeof exf_name, "%s card, exFAT part", slot_name(slot_of_dev(card_dev)));
				devs[i].name = exf_name;
			}
			if (strcmp(dest_sub, "PS2ISO") || exc_open(&dv, exf_why, sizeof exf_why))
				continue;
			exf_ok = exw_begin(&dv, dest_sub, dest_name, exf_need ? exf_need : total, exf_why, sizeof exf_why) == 0;
			devs[i].free = exw_free_bytes();
			devs[i].present = exf_ok || strcmp(exf_why, "no exFAT volume on the card");
			exw_abort();
			exc_close();
			logf_("exFAT part: %s (%s), %llu MB free", exf_ok ? "usable" : "not usable", exf_why, (unsigned long long)(devs[i].free >> 20));
			continue;
		}
		devs[i].present = is_dir(devs[i].path);
		devs[i].free = 0;
		if (devs[i].present && slot_bad(slot_of_path(devs[i].path)))
		{
			/* v0.65: a write through a slot that gives damaged data would damage the card */
			logf_("%s: not offered, the slot gives damaged data (%s)", devs[i].path, slot_cc_why[slot_of_path(devs[i].path)]);
			devs[i].present = 0;
		}
		if (ps2_img && i > 1)
			devs[i].present = 0;	/* USB drives and the other card slots: PS2 games do not start from them */
		if (!devs[i].present)
			continue;
		logf_("free space on %s", devs[i].path);
		if (sysFsGetFreeSize(devs[i].path, &bs, &fb) == 0)
			devs[i].free = (u64)bs * fb;
		logf_("  %llu MB", (unsigned long long)(devs[i].free >> 20));
	}
}

/* -1 = backed out; else the device index */
static int choose(void)
{
	int sel = -1, asking = 0, yes = 0, i;

	for (i = 0; i < NDEV; i++)
		if (usable(i)) { sel = i; break; }
	if (sel < 0)
		for (i = 0; i < NDEV; i++)
			if (devs[i].present) { sel = i; break; }
	pad_last = pad_fn ? pad_fn() : 0;	/* the Square that opened this is not a choice */
	for (;;)
	{
		unsigned p = pressed();

		if (app_events())
			return -1;
		frame++;
		if (asking)
		{
			if (p & (PAD_LEFT | PAD_RIGHT))
				yes ^= 1;
			if (p & PAD_CIRCLE)
				asking = 0;
			else if (p & PAD_CROSS)
			{
				if (yes)
					return sel;
				asking = 0;
			}
		}
		else
		{
			if (p & PAD_CIRCLE)
				return -1;
			if (p & (PAD_UP | PAD_DOWN))
			{
				int d = (p & PAD_UP) ? -1 : 1, k = sel;

				for (i = 0; i < NDEV; i++)
				{
					k = (k + d + NDEV) % NDEV;
					if (devs[k].present)
						break;
				}
				sel = k;
			}
			if ((p & PAD_CROSS) && usable(sel))
			{
				asking = 1;
				yes = 0;	/* the question starts on "No" */
			}
		}
		draw_pick(sel, asking, yes);
		gfx_present();
	}
}

int rip_disc(unsigned (*pad)(void), char *msg, int msg_n)
{
	char dst[256], body[2048];
	int r, i, d;

	pad_fn = pad;
	total = done = t_rd = t_wr = 0;
	nfiles = ndirs = nfile_at = bigfile = stop = 0;
	first_bad[0] = 0;
	t0 = tb();
	logf_("=== copy disc, app v%s", APP_VERSION);
	pick_head = "COPY DISC";
	snprintf(dest_sub, sizeof dest_sub, "GAMES");
	src_dev = -1;

	progress(NULL, "ONE MOMENT", "Looking at the disc", 0.1f, "/dev_bdvd");
	logf_("ask webMAN whether a game is mounted");
	r = wm_mounted(body, sizeof body);
	logf_("  mounted: %s %s", r ? "yes" : "no", body);
	if (r)
	{
		/* v0.39: a game image webMAN still has mounted (the last PS1 / PS3 game started) sits in
		 * front of the drive: Prism would be reading the image. It is put away here (Bananafuzzbuddy, 11-23:
		 * "the game mounted is the disk game so i don't know why it's saying that"). */
		char resp[256];
		int k;

		progress(NULL, "ONE MOMENT", "Putting the mounted game away", 0.1f, body);
		k = wm_get("/mount_ps3/unmount", resp, sizeof resp);
		logf_("  asked webMAN to unmount it (%d)", k);
		for (k = 0; k < 20 && wm_mounted(NULL, 0); k++)
			usleep(500000);
		if (wm_mounted(body, sizeof body))
		{
			snprintf(msg, msg_n, "A game image is still mounted over the drive (%s): unmount it on the XMB (webMAN), then try again", body);
			goto out;
		}
		logf_("  unmounted after %d ms", k * 500);
	}
	{
		/* v0.44: webMAN can answer "no" while Cobra still shows the last game's image in place of
		 * the drive (manage.c disc_emulated): asked of Cobra itself, and put away the same way */
		unsigned real = 0, eff = 0;
		char resp[256];
		int k;

		if (disc_emulated(&real, &eff))
		{
			logf_("Cobra shows a game image in place of the drive (real %x, shown %x): unmounting it", real, eff);
			progress(NULL, "ONE MOMENT", "Putting the mounted game away", 0.1f, "");
			k = wm_get("/mount_ps3/unmount", resp, sizeof resp);
			logf_("  asked webMAN to unmount (%d)", k);
			for (k = 0; k < 20 && disc_emulated(&real, &eff); k++)
				usleep(500000);
			logf_("  after %d ms: real %x, shown %x", k * 500, real, eff);
			if (disc_emulated(NULL, NULL))
			{
				snprintf(msg, msg_n, "A game image is still mounted over the drive: unmount it on the XMB (webMAN), then try again");
				goto out;
			}
		}
	}
	{
		/* v0.39: after an unmount the console can lose track of the disc that is really in the
		 * drive: Cobra says "none" while the drive reports a size (prism_disc.log 11-23, 4266 s).
		 * Taking the disc out and putting it back makes it known again. */
		sysFSStat st;
		st_devinfo info;

		memset(&info, 0, sizeof info);
		if (sysFsStat(BD "/PS3_GAME/PARAM.SFO", &st) != 0 && !disc_ps_type() && st_info(DRV_DEV, &info) == 0
			&& info.total_sectors && info.total_sectors < 0x7FFFFFFFull)
		{
			u64 until = tb() + 120 * TB_HZ;
			int seen = 0;

			logf_("the drive holds a disc (%llu sectors) the console does not report: asking for it to be put in again",
				(unsigned long long)info.total_sectors);
			pad_last = pad_fn ? pad_fn() : 0;
			while (tb() < until && !seen)
			{
				unsigned p = pressed();

				if (app_events() || (p & PAD_CIRCLE))
					break;
				frame++;
				backdrop();
				top_bar("COPY DISC");
				text_centre(F_BOLD, 320, 150, 0, INK, "Take the disc out and put it back in");
				text_centre(F_BODY, 320, 200, 0, DIM, "The console has lost track of the disc in the drive.");
				text_centre(F_BODY, 320, 226, 0, DIM, "Prism goes on by itself when it is back.");
				legend_line();
				legend_item(LEFT, 3, NULL, "Stop");
				gfx_present();
				seen = disc_ps_type() != 0 || sysFsStat(BD "/PS3_GAME/PARAM.SFO", &st) == 0;
			}
			if (!seen)
			{
				snprintf(msg, msg_n, "Nothing copied");
				logf_("  not put in again");
				goto out;
			}
			progress(NULL, "ONE MOMENT", "Looking at the disc", 0.1f, "/dev_bdvd");
			sleep(4);	/* the system's own look at a new disc */
			logf_("  the console reports a disc again (type %x)", disc_ps_type());
		}
	}
	{
		/* v0.32: a PS1 or PS2 disc becomes an image file (rip_ps_image) */
		char pid[12], ptitle[100];
		sysFSStat st;
		int pp;

		if (sysFsStat(BD "/PS3_GAME/PARAM.SFO", &st) != 0 && (pp = disc_ps12(pid, sizeof pid, ptitle, sizeof ptitle)) >= 0)
		{
			/* v0.38: copied the way multiMAN does it (rip_image.h, drive.c). v0.34-v0.37 refused:
			 * v0.32's own drive command had frozen the console, and without the drive reset the
			 * console gave Prism no sector of a PS2 disc (prism_disc.log 11-23). */
			logf_("a %s disc: %s [%s]", pp == P_PS2 ? "PS2" : "PS1", ptitle, pid);
			rip_ps_image(pp, disc_ps_type(), msg, msg_n);
			goto out;
		}
	}
	logf_("read the disc's PARAM.SFO");
	if (sfo_key(BD "/PS3_GAME/PARAM.SFO", "TITLE_ID", tid, sizeof tid) || strlen(tid) != 9)
	{
		snprintf(msg, msg_n, "No PS3, PS2 or PS1 game disc in the drive (put it in, wait a few seconds, try again)");
		goto out;
	}
	if (sfo_key(BD "/PS3_GAME/PARAM.SFO", "TITLE", title, sizeof title) || !title[0])
		snprintf(title, sizeof title, "%s", tid);
	logf_("  %s [%s]; check EBOOT.BIN", title, tid);
	snprintf(dest_name, sizeof dest_name, "%s", tid);
	r = self_magic(BD "/PS3_GAME/USRDIR/EBOOT.BIN");
	logf_("  EBOOT.BIN magic: %s", r == 0 ? "SCE (decrypted)" : r < 0 ? "missing" : "NOT a SELF (encrypted)");
	if (r)
	{
		snprintf(msg, msg_n, r < 0 ? "The disc has no PS3_GAME/USRDIR/EBOOT.BIN" :
			"The disc reads ENCRYPTED here (EBOOT.BIN is not a SELF): nothing copied");
		goto out;
	}

	progress(NULL, "ONE MOMENT", title, 0.2f, "Measuring the disc");
	logf_("measure the disc");
	if (measure(BD, 0))
	{
		snprintf(msg, msg_n, "Could not read the disc's folders");
		goto out;
	}
	logf_("  %d files, %d folders, %llu bytes, over 4 GB: %s", nfiles, ndirs, (unsigned long long)total, bigfile ? first_bad : "none");

	progress(NULL, "ONE MOMENT", title, 0.3f, "Looking at the storage devices");
	scan_devs();

	d = choose();
	if (d < 0)
	{
		snprintf(msg, msg_n, "Nothing copied");
		logf_("backed out");
		goto out;
	}
	buf = (u8 *)malloc(BUF_SZ);
	if (!buf)
	{
		snprintf(msg, msg_n, "Out of memory");
		goto out;
	}
	snprintf(dst, sizeof dst, "%s/GAMES", devs[d].path);
	sysFsMkdir(dst, 0777);
	snprintf(dst, sizeof dst, "%s/GAMES/%s", devs[d].path, tid);
	logf_("copy to %s", dst);
	t0 = tb();
	show("starting");
	r = copy_tree(BD, dst, 0);
	free(buf);
	buf = NULL;
	logf_("copy ended %d: %llu of %llu bytes, read %u ms, write %u ms", r, (unsigned long long)done,
		(unsigned long long)total, ms(t_rd), ms(t_wr));
	if (stop)
	{
		snprintf(msg, msg_n, "Stopped: the folder %s is only part of the game", dst);
		goto out;
	}
	if (r)
	{
		snprintf(msg, msg_n, "The copy stopped at %s", first_bad);
		goto out;
	}
	{
		char eb[300];

		snprintf(eb, sizeof eb, "%s/PS3_GAME/USRDIR/EBOOT.BIN", dst);
		if (self_magic(eb) != 0)
		{
			snprintf(msg, msg_n, "Copied, but the copy's EBOOT.BIN is not a SELF: do not use it");
			goto out;
		}
	}
	{
		double secs = (double)(tb() - t0) / TB_HZ;

		snprintf(msg, msg_n, "%s copied to %s: %d files, %.0f MB in %d:%02d", title, devs[d].name, nfiles, total / 1e6,
			(int)secs / 60, (int)secs % 60);
	}
out:
	logf_("result: %s", msg);
	return 0;
}

/* v0.37: one file into the card's exFAT part (exfat_w.c through exfat_card.c): the writer checks the
 * volume again, takes one free stretch, gets the bytes, and names the file only when all are in */
static int copy_to_exfat(const char *src, u64 size)
{
	exw_dev dv;
	char why[160];
	s32 in;
	int r = 0;
	u64 last = done;

	logf_("> exFAT %s (%llu bytes) as %s/%s", src, (unsigned long long)size, dest_sub, dest_name);
	if (exc_open(&dv, why, sizeof why) || exw_begin(&dv, dest_sub, dest_name, size, why, sizeof why))
	{
		snprintf(first_bad, sizeof first_bad, "%s", why);
		exc_close();
		return -1;
	}
	if (sysFsOpen(src, SYS_O_RDONLY, &in, NULL, 0) != 0)
	{
		exw_abort();
		exc_close();
		snprintf(first_bad, sizeof first_bad, "%s (could not open)", src);
		return -2;
	}
	for (;;)
	{
		u64 n = 0;

		if (sysFsRead(in, buf, BUF_SZ, &n) != 0)
		{
			r = -6;
			snprintf(first_bad, sizeof first_bad, "reading %s", src);
			break;
		}
		if (!n)
			break;
		if (exw_write(buf, (u32)n))
		{
			r = -4;
			snprintf(first_bad, sizeof first_bad, "writing to the card at %llu MB", (unsigned long long)(done >> 20));
			break;
		}
		done += n;
		if (done - last >= (4u << 20) || tb() - t_shown > TB_HZ / 2)
		{
			show(dest_name);
			t_shown = tb();
			last = done;
			if ((pressed() & PAD_CIRCLE) || app_events())
			{
				stop = 1;
				r = -9;
				break;
			}
		}
	}
	sysFsClose(in);
	if (!r)
	{
		if (exw_finish(why, sizeof why))
		{
			r = -7;
			snprintf(first_bad, sizeof first_bad, "%s", why);
		}
	}
	else
		exw_abort();
	exc_close();
	lr_fs_forget();		/* the card reader's cached sectors may hold the folder as it was */
	logf_("  exFAT copy ended %d (%s)", r, r ? first_bad : "named");
	return r;
}

/* ---- v0.38: one file on a mounted device, whole or as parts under 4 GiB (ripcore.c rc_parts) ----
 * Written under "<name>.part" / "<name>.<n>.part" (names the card reader does not list) and given
 * their real names only when every byte is there; on a stop or an error nothing is kept. */
static struct
{
	char dir[220], name[132];
	s32 fd;
} pk;

static int write_all_fd(s32 fd, const u8 *b, u64 n)
{
	u64 w = 0;

	return sysFsWrite(fd, b, n, &w) == 0 && w == n ? 0 : -1;
}

static void pk_path(char *o, int n, int part, int tmp)
{
	if (part < 0)
		snprintf(o, n, "%s/%s%s", pk.dir, pk.name, tmp ? ".part" : "");
	else
		snprintf(o, n, "%s/%s.%d%s", pk.dir, pk.name, part, tmp ? ".part" : "");
}

static int pk_open(void *ctx, int part)
{
	char p[400];

	(void)ctx;
	pk_path(p, sizeof p, part, 1);
	logf_("  writing %s", p);
	if (sysFsOpen(p, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &pk.fd, NULL, 0) != 0)
	{
		pk.fd = -1;
		return -1;
	}
	sysFsChmod(p, 0777);
	return 0;
}

static int pk_write(void *ctx, const uint8_t *b, uint32_t len)
{
	(void)ctx;
	return write_all_fd(pk.fd, b, len);
}

static int pk_close(void *ctx)
{
	s32 r = sysFsClose(pk.fd);

	(void)ctx;
	pk.fd = -1;
	return r != 0;
}

static void pk_begin(rc_parts *p, const char *dir, const char *name)
{
	memset(p, 0, sizeof *p);
	p->open = pk_open;
	p->write = pk_write;
	p->close = pk_close;
	snprintf(pk.dir, sizeof pk.dir, "%s", dir);
	snprintf(pk.name, sizeof pk.name, "%s", name);
	pk.fd = -1;
}

/* keep = every byte is written: an older copy under the same name goes (whole and parts alike), and
 * the new files get their names; else the unfinished files go. 0 = done. */
static int pk_finish(const rc_parts *p, int keep)
{
	char a[400], b[400];
	int i, r = 0;

	if (!keep)
	{
		for (i = -1; i < RC_PARTS_MAX; i++)
		{
			pk_path(a, sizeof a, i, 1);
			sysFsUnlink(a);
		}
		return 0;
	}
	for (i = -1; i < RC_PARTS_MAX; i++)
	{
		pk_path(a, sizeof a, i, 0);
		sysFsUnlink(a);
	}
	for (i = p->split ? 0 : -1; i < (p->split ? p->parts : 0) && !r; i++)
	{
		pk_path(a, sizeof a, i, 1);
		pk_path(b, sizeof b, i, 0);
		r = sysLv2FsRename(a, b) != 0;
		logf_("  named %s: %s", b, r ? "FAILED" : "yes");
	}
	if (r)
		for (i = -1; i < RC_PARTS_MAX; i++)
		{
			pk_path(a, sizeof a, i, 1);
			sysFsUnlink(a);
			pk_path(a, sizeof a, i, 0);
			sysFsUnlink(a);
		}
	return r ? -6 : 0;
}

/* a PS2 game file from the hard disk into dir on a FAT32 device */
static int copy_to_parts(const char *src, const char *dir, const char *name, u64 size)
{
	rc_parts p;
	s32 in;
	int r = 0;
	u64 last = done;

	logf_("> %s (%llu bytes) to %s/%s%s", src, (unsigned long long)size, dir, name, size > RC_WHOLE_MAX ? " in parts" : "");
	pk_begin(&p, dir, name);
	if (rc_parts_begin(&p, size, RC_PART_MAX, RC_WHOLE_MAX))
	{
		snprintf(first_bad, sizeof first_bad, "%s (too big for %d parts)", src, RC_PARTS_MAX);
		return -1;
	}
	if (sysFsOpen(src, SYS_O_RDONLY, &in, NULL, 0) != 0)
	{
		snprintf(first_bad, sizeof first_bad, "%s (could not open)", src);
		return -2;
	}
	while (!r)
	{
		u64 n = 0;

		if (sysFsRead(in, buf, BUF_SZ, &n) != 0)
		{
			r = -6;
			snprintf(first_bad, sizeof first_bad, "reading %s", src);
			break;
		}
		if (!n)
			break;
		if (rc_parts_write(&p, buf, (u32)n))
		{
			r = -4;
			snprintf(first_bad, sizeof first_bad, "writing to the card at %llu MB", (unsigned long long)(done >> 20));
			break;
		}
		done += n;
		if (done - last >= (4u << 20) || tb() - t_shown > TB_HZ / 2)
		{
			show(name);
			t_shown = tb();
			last = done;
			if ((pressed() & PAD_CIRCLE) || app_events())
			{
				stop = 1;
				r = -9;
			}
		}
	}
	sysFsClose(in);
	if (rc_parts_end(&p) && !r)
	{
		r = -5;
		snprintf(first_bad, sizeof first_bad, "the copy of %s is not whole", name);
	}
	if (pk_finish(&p, r == 0) && !r)
	{
		r = -7;
		snprintf(first_bad, sizeof first_bad, "naming %s on the card", name);
	}
	lr_fs_forget();
	logf_("  copy in parts ended %d: %d file(s)", r, p.parts);
	return r;
}

/* ---- v0.15: copy a PS1 / PS3 game from one storage device to another ---- */

static char flist[64][256];

static int where_dev(int where)
{
	if (where == 0)
		return 0;
	if (where == 2)
		return 1;
	if (where >= 3 && where <= 10)
		return 2 + (where - 3);
	if (where == WHERE_MS)
		return 10;
	if (where == WHERE_CF)
		return 11;
	return -1;
}

/* ---- v0.56: a PS2 game on the SD card copied to the hard disk (Bananafuzzbuddy, 2026-11-24) ----
 * The card's games are read through Prism's own card reader (lr_fs.c, the stage2's code), so a game
 * on the exFAT part, in /PS2PRISM or split into .0/.1 parts arrives as ONE file in /dev_hdd0/PS2ISO,
 * where webMAN and Prism both play it. Written as <name>.part and renamed when whole: a stopped or
 * failed copy never shows up as a game. The card is only read. */
static int copy_card_ps2(const game_t *g, char *msg, int msg_n)
{
	const char *base = strrchr(g->path, '/');
	char dst[300], part[310];
	u64 size = 0, off, last = 0, freeb = 0;
	u32 bs = 0;
	s32 out = -1;
	int r;

	base = base ? base + 1 : g->path;
	snprintf(dst, sizeof dst, "/dev_hdd0/PS2ISO/%s", base);
	snprintf(part, sizeof part, "%s.part", dst);
	logf_("=== copy card PS2 game, app v%s: %s -> %s", APP_VERSION, g->path, dst);
	{
		sysFSStat st;

		if (sysFsStat(dst, &st) == 0)
		{
			snprintf(msg, msg_n, "%s is already on the hard disk", g->shown);
			last_result = 2;
			return 0;
		}
	}
	progress(NULL, "ONE MOMENT", title, 0.2f, "Opening the game on the card");
	r = fsport_attach(slot_storage(g->slot));	/* v0.66: the game's own slot (v0.63: the one card's slot) */
	if (r == 0)
		r = lr_fs_mount();
	if (r == 0)
		r = lr_fs_open(g->path, &size);
	if (r || !size)
	{
		fsport_detach();
		snprintf(msg, msg_n, "Could not open the game on the %s card (%d)", slot_name(g->slot), r);
		return 0;
	}
	total = size;
	nfiles = 1;
	if (sysFsGetFreeSize("/dev_hdd0", &bs, &freeb) == 0 && (u64)bs * freeb < size + (64ull << 20))
	{
		fsport_detach();
		snprintf(msg, msg_n, "Not enough room on the hard disk: %.1f GB needed, %.1f GB free", size / 1073741824.0,
			(double)bs * freeb / 1073741824.0);
		return 0;
	}
	buf = (u8 *)malloc(BUF_SZ);
	sysFsMkdir("/dev_hdd0/PS2ISO", 0777);
	if (!buf || sysFsOpen(part, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &out, NULL, 0) != 0)
	{
		int nomem = !buf;

		fsport_detach();
		free(buf);
		buf = NULL;
		snprintf(msg, msg_n, nomem ? "Out of memory" : "Could not write to the hard disk");
		return 0;
	}
	sysFsChmod(part, 0777);
	show_head = "COPYING TO THE HARD DISK";
	t0 = tb();
	show("starting");
	for (off = 0; off < size && !r; )
	{
		u32 n = size - off < BUF_SZ ? (u32)(size - off) : BUF_SZ;
		u64 w = 0;

		if (lr_file_read(off, buf, n))
			r = -6;
		else if (sysFsWrite(out, buf, n, &w) != 0 || w != n)
			r = -3;
		off += n;
		done = off;
		if (!r && (done - last >= (4u << 20) || tb() - t_shown > TB_HZ / 2))
		{
			show(base);
			t_shown = tb();
			last = done;
			if ((pressed() & PAD_CIRCLE) || app_events())
			{
				stop = 1;
				r = -9;
			}
		}
	}
	sysFsClose(out);
	fsport_detach();
	free(buf);
	buf = NULL;
	show_head = "COPYING THE DISC";
	logf_("card copy ended %d: %llu of %llu bytes", r, (unsigned long long)done, (unsigned long long)total);
	if (r == 0 && sysLv2FsRename(part, dst) != 0)
		r = -4;
	if (r)
	{
		sysFsUnlink(part);
		if (r == -9)
			snprintf(msg, msg_n, "Stopped: nothing kept on the hard disk");
		else
			snprintf(msg, msg_n, "The copy stopped at %.0f MB (%s, %d): nothing kept", done / 1e6, r == -6 ? "card read" : "disk write", r);
	}
	else
	{
		double secs = (double)(tb() - t0) / TB_HZ;

		snprintf(msg, msg_n, "%s copied to the hard disk: %.0f MB in %d:%02d", g->shown, total / 1e6, (int)secs / 60, (int)secs % 60);
		last_result = 1;
	}
	return 0;
}

int copy_game(unsigned (*pad)(void), int plat, const game_t *g, char *msg, int msg_n)
{
	char dst[300], m[300];
	const char *base = strrchr(g->path, '/');
	int r = 0, i, d, n = 0, tree = plat == P_PS3 && g->kind == 3;

	last_result = 0;
	stop = 0;
	if (slot_bad_msg(slot_of_game(g), "Not copied", msg, msg_n))	/* v0.65: the copy would carry the slot's damage */
		return -1;
	if (plat == P_PS2 && g->where == 1 && g->kind < 4)
	{
		/* v0.56: the card's PS2 games go to the hard disk */
		pad_fn = pad;
		total = done = t_rd = t_wr = 0;
		nfiles = ndirs = nfile_at = bigfile = stop = 0;
		first_bad[0] = 0;
		t0 = tb();
		if (batch_n)
			snprintf(title, sizeof title, "%d of %d: %s", batch_k, batch_n, g->shown);	/* v0.67 */
		else
			snprintf(title, sizeof title, "%s", g->shown);
		pad_last = pad_fn ? pad_fn() : 0;	/* the X that chose this is not a Circle */
		r = copy_card_ps2(g, msg, msg_n);
		logf_("result: %s", msg);
		return r;
	}
	pad_fn = pad;
	total = done = t_rd = t_wr = 0;
	nfiles = ndirs = nfile_at = bigfile = stop = 0;
	first_bad[0] = 0;
	t0 = tb();
	logf_("=== copy game, app v%s: %s", APP_VERSION, g->path);
	pick_head = "COPY GAME";
	src_dev = where_dev(g->where);
	if (batch_n)
		snprintf(title, sizeof title, "%d of %d: %s", batch_k, batch_n, g->shown);	/* v0.67 */
	else
		snprintf(title, sizeof title, "%s", g->shown);
	snprintf(tid, sizeof tid, "%s", g->id[0] ? g->id : "-");
	base = base ? base + 1 : g->path;
	if ((plat == P_PS2 && g->where != 2) || g->kind >= 4)
	{
		snprintf(msg, msg_n, "Only PS1, PSP and PS3 games, and PS2 games on the hard disk, are copied this way");
		goto out;
	}
	if (wm_mounted(m, sizeof m) && (strstr(m, g->path) || strstr(g->path, m)))
	{
		snprintf(msg, msg_n, "It is mounted by webMAN right now: unmount it on the XMB first");
		goto out;
	}
	progress(NULL, "ONE MOMENT", title, 0.2f, "Measuring the game");
	if (tree)
	{
		if (measure(g->path, 0))
		{
			snprintf(msg, msg_n, "Could not read the game's folders");
			goto out;
		}
		snprintf(dest_sub, sizeof dest_sub, "GAMES");
	}
	else
	{
		n = game_files(plat, g, flist, 64);
		for (i = 0; i < n; i++)
		{
			sysFSStat st;

			if (sysFsStat(flist[i], &st) == 0)
			{
				total += st.st_size;
				nfiles++;
				if (st.st_size > FAT32_MAX && !bigfile)
				{
					bigfile = 1;
					snprintf(first_bad, sizeof first_bad, "%s", flist[i]);
				}
			}
		}
		if (!n)
		{
			snprintf(msg, msg_n, "Could not find the game's files");
			goto out;
		}
		snprintf(dest_sub, sizeof dest_sub, plat == P_PS1 ? "PSXISO" : plat == P_PSP ? "PSPISO" : plat == P_PS2 ? "PS2ISO" : "PS3ISO");
	}
	snprintf(dest_name, sizeof dest_name, "%s", base);
	logf_("  %d files, %llu bytes", nfiles, (unsigned long long)total);
	link_hit = 0;
	progress(NULL, "ONE MOMENT", title, 0.3f, "Looking at the storage devices");
	ps2_img = plat == P_PS2;
	scan_devs();
	if (batch_d >= 0)
	{
		/* v0.67: the batch's device, chosen once; looked at again for THIS game: room, and whether it
		 * is the device the game is on */
		d = batch_d;
		if (d == src_dev)
		{
			snprintf(msg, msg_n, "it is already on %s", devs[d].name);
			last_result = 2;
			goto out;
		}
		if (!usable(d))
		{
			if (d == EXF && exf_why[0])
				snprintf(msg, msg_n, "%s", exf_why);
			else if (devs[d].fat && bigfile && !(d == 0 && ps2_img))
				snprintf(msg, msg_n, "a file is over 4 GB: %s cannot hold it", devs[d].name);
			else
				snprintf(msg, msg_n, "no room for it on %s", devs[d].name);
			goto out;
		}
	}
	else
		d = choose();
	if (d < 0)
	{
		snprintf(msg, msg_n, "Nothing copied");
		goto out;
	}
	buf = (u8 *)malloc(BUF_SZ);
	if (!buf)
	{
		snprintf(msg, msg_n, "Out of memory");
		goto out;
	}
	if (d != EXF)
	{
		snprintf(dst, sizeof dst, "%s/%s", devs[d].path, sub_on(d));
		sysFsMkdir(dst, 0777);
	}
	t0 = tb();
	show("starting");
	if (tree)
	{
		snprintf(dst, sizeof dst, "%s/%s/%s", devs[d].path, dest_sub, base);
		logf_("copy to %s", dst);
		r = copy_tree(g->path, dst, 0);
	}
	else
		for (i = 0; i < n && !r; i++)
		{
			const char *b = strrchr(flist[i], '/');
			sysFSStat st;

			snprintf(dst, sizeof dst, "%s/%s/%s", devs[d].path, dest_sub, b ? b + 1 : flist[i]);
			if (sysFsStat(flist[i], &st) != 0)
				continue;
			if (d == EXF)
			{
				snprintf(dest_name, sizeof dest_name, "%s", b ? b + 1 : flist[i]);
				r = copy_to_exfat(flist[i], st.st_size);
			}
			else if (ps2_img && d == 0)
			{
				/* the card's FAT32 part: /PS2PRISM, in parts when it is over 4 GiB */
				snprintf(dst, sizeof dst, "%s/%s", devs[d].path, PS2_FAT_DIR);
				r = copy_to_parts(flist[i], dst, b ? b + 1 : flist[i], st.st_size);
			}
			else
				r = copy_file(flist[i], dst, st.st_size);
			if (!r && stop)
				r = -9;
			if (r && r != -9)
				snprintf(first_bad, sizeof first_bad, "%s (%d)", flist[i], r);
		}
	free(buf);
	buf = NULL;
	logf_("copy ended %d: %llu of %llu bytes", r, (unsigned long long)done, (unsigned long long)total);
	if (stop && ps2_img && (d == 0 || d == EXF))
		snprintf(msg, msg_n, "Stopped: nothing kept on the card");
	else if (stop)
		snprintf(msg, msg_n, "Stopped: the copy on %s is only part of the game", devs[d].name);
	else if (r && link_hit)
		snprintf(msg, msg_n, "PPSSPP opened this game from the card, and its name on the hard disk is held until the console restarts. Restart the console, then copy it");
	else if (r)
		snprintf(msg, msg_n, "The copy stopped at %s", first_bad);
	else
	{
		double secs = (double)(tb() - t0) / TB_HZ;

		snprintf(msg, msg_n, "%s copied to %s: %.0f MB in %d:%02d", title, devs[d].name, total / 1e6, (int)secs / 60, (int)secs % 60);
		last_result = 1;
	}
out:
	logf_("result: %s", msg);
	src_dev = -1;
	ps2_img = 0;
	return 0;
}

/* ---- v0.67: several games in one go (Bananafuzzbuddy, 2026-11-24: "select ten of them and have them transfer all
 * over to the SD card at once, and vice versa") ----
 * The games are the ones marked in a list (screens.c, R2). They are measured, the device is chosen ONCE
 * (the same picker, the sum of their sizes against each device's room), one question is asked, and then
 * copy_game() runs for each as it does for a single game: every rule it has (the bad-slot refusal, .part
 * files, parts under 4 GB on FAT32, the exFAT writer's checks) holds for each game of the batch.
 * PS2 games on a card go to the hard disk (the only place copy_card_ps2 writes); a batch that holds both
 * card and hard-disk PS2 games is refused, since they would go opposite ways.
 * Circle stops the game being copied AND the rest of the batch. */
static int ask_hdd(int n, u64 sum, u64 freeb)
{
	char l1[120], l2[120], head[60];

	snprintf(head, sizeof head, "Copy %d games to the hard disk?", n);
	snprintf(l1, sizeof l1, "%.1f GB in all, %.1f GB free on the internal hard disk.", sum / 1073741824.0, freeb / 1073741824.0);
	snprintf(l2, sizeof l2, "They stay on the card too. Circle: back.");
	pad_last = pad_fn ? pad_fn() : 0;
	for (;;)
	{
		unsigned p = pressed();

		if (app_events() || (p & PAD_CIRCLE))
			return 0;
		if (p & PAD_CROSS)
			return 1;
		frame++;
		backdrop();
		draw_message(head, l1, l2, "X", "Copy them");
		gfx_present();
	}
}

int copy_games(unsigned (*pad)(void), int plat, const game_t *list, int n, char *msg, int msg_n)
{
	static const char *const pn[N_PLAT] = { "PS1", "PS2", "PS3", "PSP" };
	char m[300], why1[220], where[40];
	int k, d = -1, card = 0, other = 0, common = -2, anybig = 0, ok = 0, there = 0, bad = 0, stopped = 0;
	u64 sum = 0, biggest = 0, t_all;

	why1[0] = 0;
	if (n <= 0)
	{
		snprintf(msg, msg_n, "No games are marked");
		return -1;
	}
	pad_fn = pad;
	for (k = 0; k < n; k++)
	{
		if (plat == P_PS2 && list[k].where == 1 && list[k].kind < 4)
			card++;
		else
			other++;
	}
	logf_("=== copy %d marked %s games, app v%s (%d on a card read by Prism)", n, pn[plat], APP_VERSION, card);
	if (card && other)
	{
		snprintf(msg, msg_n, "Nothing copied: some marked games are on a card and some on the hard disk. Mark one kind at a time.");
		return -1;
	}
	if (card)
	{
		u64 freeb = 0;
		u32 bs = 0;

		for (k = 0; k < n; k++)
			sum += list[k].size;
		if (sysFsGetFreeSize("/dev_hdd0", &bs, &freeb) != 0)
			freeb = 0;
		freeb *= bs;
		if (freeb < sum + (64ull << 20))
		{
			snprintf(msg, msg_n, "Nothing copied: not enough room on the hard disk, %.1f GB needed, %.1f GB free", sum / 1073741824.0, freeb / 1073741824.0);
			return -1;
		}
		if (!ask_hdd(n, sum, freeb))
		{
			snprintf(msg, msg_n, "Nothing copied");
			return -1;
		}
		snprintf(where, sizeof where, "the hard disk");
	}
	else
	{
		/* measure every game: the picker judges each device against all of them together */
		for (k = 0; k < n; k++)
		{
			const game_t *g = &list[k];
			int dev = where_dev(g->where), i, nf;

			snprintf(m, sizeof m, "Measuring game %d of %d", k + 1, n);
			progress(NULL, "ONE MOMENT", g->shown, 0.1f + 0.3f * k / n, m);
			if (g->kind >= 4)
				continue;	/* a disc: copy_game says so when its turn comes */
			total = 0;
			nfiles = ndirs = bigfile = 0;
			first_bad[0] = 0;
			if (plat == P_PS3 && g->kind == 3)
				measure(g->path, 0);
			else
			{
				nf = game_files(plat, g, flist, 64);
				for (i = 0; i < nf; i++)
				{
					sysFSStat st;

					if (sysFsStat(flist[i], &st) == 0)
					{
						total += st.st_size;
						if (st.st_size > FAT32_MAX)
							bigfile = 1;
						if (st.st_size > biggest)
							biggest = st.st_size;
					}
				}
			}
			sum += total;
			anybig |= bigfile;
			common = common == -2 ? dev : common == dev ? dev : -1;
			if (app_events())
			{
				snprintf(msg, msg_n, "Nothing copied");
				return -1;
			}
		}
		{
			const char *base = strrchr(list[0].path, '/');

			snprintf(dest_name, sizeof dest_name, "%s", base ? base + 1 : list[0].path);
		}
		if (plat == P_PS3 && list[0].kind == 3)
			snprintf(dest_sub, sizeof dest_sub, "GAMES");
		else
			snprintf(dest_sub, sizeof dest_sub, plat == P_PS1 ? "PSXISO" : plat == P_PSP ? "PSPISO" : plat == P_PS2 ? "PS2ISO" : "PS3ISO");
		total = sum;
		bigfile = anybig;
		exf_need = biggest;
		src_dev = common < 0 ? -1 : common;
		ps2_img = plat == P_PS2;
		snprintf(title, sizeof title, "%d %s games", n, pn[plat]);
		snprintf(tid, sizeof tid, "-");
		pick_head = "COPY GAMES";
		pick_many = 1;
		logf_("  %llu bytes in all, the largest file %llu", (unsigned long long)sum, (unsigned long long)biggest);
		progress(NULL, "ONE MOMENT", title, 0.5f, "Looking at the storage devices");
		scan_devs();
		d = choose();
		pick_many = 0;
		exf_need = 0;
		src_dev = -1;
		ps2_img = 0;
		if (d < 0)
		{
			snprintf(msg, msg_n, "Nothing copied");
			return -1;
		}
		snprintf(where, sizeof where, "%s", devs[d].name);
	}
	batch_n = n;
	batch_d = d;
	t_all = tb();
	for (k = 0; k < n && !stopped; k++)
	{
		batch_k = k + 1;
		m[0] = 0;
		copy_game(pad, plat, &list[k], m, sizeof m);
		logf_("batch %d of %d: result %d: %s", k + 1, n, last_result, m);
		if (last_result == 1)
			ok++;
		else if (last_result == 2)
			there++;
		else
		{
			bad++;
			if (!why1[0])
				snprintf(why1, sizeof why1, "%.60s: %.150s", list[k].shown, m);
		}
		if (stop || app_events())
			stopped = 1;
	}
	batch_n = 0;
	batch_d = -1;
	{
		double secs = (double)(tb() - t_all) / TB_HZ;
		int left = n - ok - there - bad;

		if (stopped)
			snprintf(msg, msg_n, "Stopped: %d of %d copied to %s, %d not started", ok, n, where, left);
		else if (!bad && !there)
			snprintf(msg, msg_n, "All %d games copied to %s in %d:%02d", n, where, (int)secs / 60, (int)secs % 60);
		else if (!bad)
			snprintf(msg, msg_n, "%d copied to %s, %d were already there", ok, where, there);
		else
			snprintf(msg, msg_n, "%d of %d copied to %s. Not copied: %s", ok, n, where, why1);
	}
	logf_("batch result: %s", msg);
	return bad || stopped ? -1 : 0;
}

/* ---- v0.30: a PSP game on the SD card runs from a copy on the hard disk ----
 * From /dev_sd the PSP launcher starts (cellGameExecGame returned 0, prism_start.log 11-23) but
 * the game does not run (Bananafuzzbuddy, 11-23) - the card slot's second failure after PS3 games
 * (Borderlands, v0.22). The game's ISO is copied once to PSP_CACHE (a "/PSPISO" folder: webMAN's
 * /mount_ps3 treats a file as a PSP UMD only with "/PSPISO" in its path, mount.h) and started from
 * there; the copy is written as <name>.part and renamed when whole, so a stopped copy is never
 * used, and a later start of the same game reuses it. Only the latest game is kept there. */
#define PSP_CACHE	"/dev_hdd0/tmp/prism_psp/PSPISO"

int psp_card_copy(unsigned (*pad)(void), const game_t *g, char *path, int path_n, char *msg, int msg_n)
{
	const char *base = strrchr(g->path, '/');
	char dst[300], part[310];
	sysFSStat st, sd;
	dl_ent *ents = NULL;
	int r, i, n;
	u32 bs = 0;
	u64 fb = 0;

	base = base ? base + 1 : g->path;
	snprintf(dst, sizeof dst, "%s/%s", PSP_CACHE, base);
	snprintf(part, sizeof part, "%s.part", dst);
	snprintf(path, path_n, "%s", dst);
	if (sysFsStat(g->path, &st) != 0)
	{
		snprintf(msg, msg_n, "Could not read the game on the SD card");
		return -1;
	}
	if (sysFsStat(dst, &sd) == 0 && sd.st_size == st.st_size)
	{
		logf_("psp card copy: %s already on the hard disk", base);
		return 0;
	}
	sysFsMkdir("/dev_hdd0/tmp/prism_psp", 0777);
	sysFsMkdir(PSP_CACHE, 0777);
	/* only the latest game stays: the others (and any stopped copy) go */
	n = dir_list(PSP_CACHE, &ents);
	for (i = 0; i < n; i++)
	{
		char old[600];

		if (ents[i].dir)
			continue;
		snprintf(old, sizeof old, "%s/%s", PSP_CACHE, ents[i].name);
		sysFsUnlink(old);
		logf_("psp card copy: removed %s", old);
	}
	free(ents);
	if (sysFsGetFreeSize("/dev_hdd0", &bs, &fb) == 0 && (u64)bs * fb < st.st_size + (256u << 20))
	{
		snprintf(msg, msg_n, "Not started: the hard disk needs %.0f MB free to play it from the SD card", (st.st_size + (256u << 20)) / 1e6);
		return -2;
	}
	buf = (u8 *)malloc(BUF_SZ);
	if (!buf)
	{
		snprintf(msg, msg_n, "Out of memory");
		return -3;
	}
	pad_fn = pad;
	pad_last = pad ? pad() : 0;
	total = st.st_size;
	done = t_rd = t_wr = 0;
	nfiles = 1;
	nfile_at = ndirs = bigfile = stop = 0;
	first_bad[0] = 0;
	snprintf(title, sizeof title, "%s", g->shown);
	show_head = "COPYING TO THE HARD DISK";	/* once per game; kept for the next start */
	t0 = tb();
	logf_("=== psp card copy, app v%s: %s -> %s", APP_VERSION, g->path, dst);
	show("starting");
	r = copy_file(g->path, part, st.st_size);
	free(buf);
	buf = NULL;
	show_head = "COPYING THE DISC";
	if (!r && stop)
		r = -9;
	if (!r && (sysFsStat(part, &sd) != 0 || sd.st_size != st.st_size))
		r = -7;
	if (!r && sysLv2FsRename(part, dst) != 0)
		r = -8;
	logf_("psp card copy ended %d: %llu of %llu bytes in %.1f s", r, (unsigned long long)done, (unsigned long long)total,
		(double)(tb() - t0) / TB_HZ);
	if (r)
	{
		sysFsUnlink(part);
		if (r == -9)
			snprintf(msg, msg_n, "Stopped: not started");
		else
			snprintf(msg, msg_n, "Not started: the copy to the hard disk failed (%d)", r);
		return r;
	}
	return 0;
}

/* ---- v0.38: a PS1 / PS2 disc copied to an image file ---- */
#include "rip_image.h"
