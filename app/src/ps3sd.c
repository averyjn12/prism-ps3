/* v0.82: a PS3 game ISO in a card slot, played from the card itself (Bananafuzzbuddy 2026-11-27: "get the exfat to work for ps3
 * games", on the card's exFAT part as well as its FAT32 part). lr/re_notes/ps3_from_sd_plan_2026-11-27.md.
 *
 * Why not simply mount it: a game started from a /dev_sd file black-screens (Borderlands from /dev_sd/GAMES, v0.22): a
 * game's own process cannot open the card slot, and GameOS does not see the exFAT part at all. webMAN plays PS3 ISOs
 * from NTFS / exFAT USB drives through rawseciso, which reads the drive's raw sectors from the VSH (where they CAN be
 * read: the PSP route, pspsd.c, started a game that way on 2026-11-24) and serves /dev_bdvd; the game only ever reads
 * /dev_bdvd.
 *
 *   1. Prism's card reader (lr_fs.c, read-only) opens the ISO by the card's raw sectors (whole, or split .0 .1 ...) and
 *      lists its runs; the image must be a disc: ISO 9660 at sector 16 with PS3_GAME/PARAM.SFO in it. A whole file
 *      the system can also see (the FAT32 part) must read the same both ways at 10 places, as pspsd.c checks.
 *   2. The .ntfs[PS3ISO] file (pspsd_core.c ps3_args) goes to /dev_hdd0/tmp/prism_ps3 (NOT webMAN's wmtmp: its NTFS
 *      scan deletes every .ntfs[PS3ISO] there on each refresh) and webMAN mounts it.
 *   3. /dev_bdvd/PS3_GAME/PARAM.SFO must be the card's PARAM.SFO, byte for byte. Only then does launch.c start the game.
 * Any failure: webMAN unmounts, nothing is started. Each step goes to /dev_hdd0/tmp/prism_ps3sd.log. The card is never
 * written. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <lv2/sysfs.h>
#include <sys/file.h>
#include "games.h"
#include "storage.h"
#include "webman.h"
#include "lr.h"
#include "fsport.h"
#include "pspsd_core.h"
#include "ps3sd.h"
#include "version.h"

int lr_fs_run(u32 i, u32 *lba, u32 *n);	/* lr_fs.c, LR_APP */
int lr_fs_open_exfat(const char *path, u64 *size);	/* lr_fs.c, LR_APP (1.00): the exFAT volume(s) only */
int sfo_get(const u8 *d, int n, const char *key, char *out, int out_n);	/* games.c */

#define DIR_	"/dev_hdd0/tmp/prism_ps3"
#define NTFS	DIR_ "/card.ntfs[PS3ISO]"
#define LOGF	"/dev_hdd0/tmp/prism_ps3sd.log"
#define SFO_BD	"/dev_bdvd/PS3_GAME/PARAM.SFO"
#define MOUNTED	DIR_ "/mounted"	/* v0.82: the title ID of the game Prism last mounted from a card */
#define CHUNK	(64 * 1024)
#define SFO_MAX	(64 * 1024)
#define DIR_MAX	(64 * 1024)
#define MAX_RUNS	((FI_ARGS_MAX - 20) / 8)

static FILE *lg;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	va_list ap;

	if (!lg)
		return;
	va_start(ap, fmt);
	vfprintf(lg, fmt, ap);
	va_end(ap);
	fputc('\n', lg);
	fflush(lg);
}

static u32 le32_(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

/* the record called name (case-insensitive, ";1" ignored) in the directory at lba / len of the open image. 0 */
static int iso_find_(u8 *buf, u32 lba, u32 len, const char *name, u32 *out_lba, u32 *out_len, int *is_dir)
{
	u32 o = 0, nl = (u32)strlen(name);

	if (len > DIR_MAX)
		len = DIR_MAX;
	if (lr_file_read((u64)lba * 2048, buf, len))
		return -1;
	while (o + 34 <= len)
	{
		u32 rl = buf[o], k, n;
		const u8 *id;

		if (rl == 0)
		{
			o = (o / 2048 + 1) * 2048;	/* records do not cross sectors */
			continue;
		}
		if (o + rl > len || rl < 34)
			return -1;
		n = buf[o + 32];
		id = buf + o + 33;
		if (n >= 2 && id[n - 2] == ';')
			n -= 2;
		if (n == nl)
		{
			for (k = 0; k < n && (id[k] | 0x20) == (name[k] | 0x20); k++)
				;
			if (k == n)
			{
				*out_lba = le32_(buf + o + 2);
				*out_len = le32_(buf + o + 10);
				*is_dir = (buf[o + 25] & 2) != 0;
				return 0;
			}
		}
		o += rl;
	}
	return -1;
}

static int write_file(const char *path, const void *d, u32 n)
{
	FILE *f = fopen(path, "wb");
	int ok;

	if (!f)
		return -1;
	ok = fwrite(d, 1, n, f) == n;
	return fclose(f) == 0 && ok ? 0 : -1;
}

static int sys_read(const char *path, u64 off, u8 *buf, u32 len, u32 *got_out)
{
	s32 fd;
	u64 got = 0, pos;
	int r = -1;

	if (sysFsOpen(path, SYS_O_RDONLY, &fd, NULL, 0) != 0)
		return -1;
	if (sysFsLseek(fd, (s64)off, 0, &pos) == 0 && sysFsRead(fd, buf, len, &got) == 0)
		r = 0;
	sysFsClose(fd);
	if (got_out)
		*got_out = (u32)got;
	return r;
}

/* v0.82 (Bananafuzzbuddy 11-27: quitting a PS3 game played from the card's FAT32 part restarted the console once; cause not
 * known): when Prism starts again and webMAN is still serving the game Prism mounted from a card (the same TITLE_ID
 * on /dev_bdvd), it is unmounted before anything reads a card, so the card is never read by rawseciso and by Prism at
 * the same time. A disc mounted some other way since is left alone. */
void ps3sd_release(void)
{
	static u8 sfo[8192];
	char want[16], have[16], resp[200];
	s32 fd;
	u64 got = 0;
	int n, r;
	FILE *f = fopen(MOUNTED, "rb");

	if (!f)
		return;
	n = (int)fread(want, 1, sizeof want - 1, f);
	fclose(f);
	want[n > 0 ? n : 0] = 0;
	sysFsUnlink(MOUNTED);
	have[0] = 0;
	if (sysFsOpen(SFO_BD, SYS_O_RDONLY, &fd, NULL, 0) == 0)
	{
		sysFsRead(fd, sfo, sizeof sfo, &got);
		sysFsClose(fd);
		if (got)
			sfo_get(sfo, (int)got, "TITLE_ID", have, sizeof have);
	}
	lg = fopen(LOGF, "a");
	if (want[0] && !strcmp(want, have))
	{
		r = wm_get("/mount_ps3/unmount", resp, sizeof resp);
		say("=== Prism started with %s still served from the card: webMAN unmount %d", want, r);
	}
	else
		say("=== Prism started: the card's %s is no longer on /dev_bdvd (%s there): nothing to unmount", want, have[0] ? have : "nothing");
	if (lg)
		fclose(lg);
	lg = NULL;
}

int ps3sd_card_game(const void *gv)
{
	const game_t *g = (const game_t *)gv;

	/* v0.82: a card slot's (either part), or a USB drive's exFAT volume (a FAT32 USB drive is mounted the usual way) */
	return g->kind == 0 && (WHERE_SLOT(g->where) || (g->exfat && g->where >= 3 && g->where <= 10));
}

int ps3sd_prepare(const void *gv, char *why, int wn)
{
	const game_t *g = (const game_t *)gv;
	u64 dev = game_raw_dev(g);	/* v0.82: a card slot or a USB port (games.c) */
	const char *slash;
	char dir[16];
	static uint32_t lba[MAX_RUNS], cnt[MAX_RUNS];
	static u8 args[FI_ARGS_MAX];
	char cpath[208], enc[300], url[340], resp[200], tid[16];
	u8 *a = NULL, *b = NULL, *d = NULL;
	u32 r_lba, r_len, s_lba = 0, s_len = 0, got = 0;
	sysFSStat st;
	u64 size = 0;
	int r, k, nr = 0, len, attached = 0, isdir, whole_sys;
	size_t L;

	lg = fopen(LOGF, "a");
	say("=== PS3 game from the card, app v%s: %s (%s part)", APP_VERSION, g->path, g->exfat ? "exFAT" : "FAT32");
	/* the device's GameOS name is the path's first part ("/dev_sd", "/dev_usb003") */
	slash = g->path[0] == '/' ? strchr(g->path + 1, '/') : NULL;
	L = slash ? (size_t)(slash - g->path) : 0;
	if (!dev || !L || L >= sizeof dir)
	{
		snprintf(why, wn, "not on a card or a USB drive Prism reads");
		goto fail;
	}
	memcpy(dir, g->path, L);
	dir[L] = 0;
	/* the card reader's path: no slot prefix, and a split set by its name without ".0" */
	snprintf(cpath, sizeof cpath, "%s", g->path + L);
	k = (int)strlen(cpath);
	if (k > 6 && !strcmp(cpath + k - 2, ".0"))
		cpath[k - 2] = 0;
	whole_sys = !g->exfat && strcmp(cpath, g->path + L) == 0 && sysFsStat(g->path, &st) == 0 && st.st_size > 0;
	a = (u8 *)malloc(CHUNK);
	b = (u8 *)malloc(CHUNK);
	d = (u8 *)malloc(DIR_MAX > SFO_MAX ? DIR_MAX : SFO_MAX);
	if (!a || !b || !d)
	{
		snprintf(why, wn, "out of memory");
		goto fail;
	}

	/* 1. the runs, by the card's raw sectors */
	fsport_log = lg;
	r = fsport_attach(dev);
	say("  device %s (%llx): open %d", dir, (unsigned long long)dev, r);
	attached = r == 0;
	if (r == 0)
	{
		r = lr_fs_mount();
		say("  partitions: %d", r);
	}
	if (r == 0)
	{
		/* 1.00: a game listed from the exFAT part is opened THERE: a FAT32 file of the same name is found first otherwise */
		r = g->exfat ? lr_fs_open_exfat(cpath, &size) : lr_fs_open(cpath, &size);
		say("  file %s: %d, %llu bytes%s", cpath, r, (unsigned long long)size, g->exfat ? " (exFAT part)" : "");
	}
	fsport_log = NULL;
	if (r || !size || (size & 2047) || (whole_sys && size != (u64)st.st_size))
	{
		snprintf(why, wn, "the card reader could not open it in %s (%s, %d)", dir,
			!attached ? "the slot" : r ? "the partitions or the file" : (size & 2047) ? "not whole 2048-byte sectors" : "a different size", r);
		goto fail;
	}
	for (nr = 0; nr < MAX_RUNS && lr_fs_run((u32)nr, &lba[nr], &cnt[nr]) > 0; nr++)
		;
	if (nr == 0 || lr_fs_run((u32)nr, &lba[0], &cnt[0]) > 0)
	{
		snprintf(why, wn, "%s", nr ? "the game is in too many pieces on the card" : "no runs");
		goto fail;
	}
	/* lr_fs_run's last call above overwrote lba[0] / cnt[0] only when it returned > 0, which is a failure */
	say("  %llu bytes in %d run(s), first at card sector %u", (unsigned long long)size, nr, lba[0]);

	/* the image is a PS3 disc: ISO 9660 at sector 16, PS3_GAME/PARAM.SFO in it */
	if (lr_file_read(16 * 2048, a, 2048) || a[0] != 1 || memcmp(a + 1, "CD001", 5))
	{
		snprintf(why, wn, "not a disc image (no ISO 9660 volume at sector 16)");
		goto fail;
	}
	if (iso_find_(d, le32_(a + 156 + 2), le32_(a + 156 + 10), "PS3_GAME", &r_lba, &r_len, &isdir) || !isdir
	    || iso_find_(d, r_lba, r_len, "PARAM.SFO", &s_lba, &s_len, &isdir) || isdir || !s_len || s_len > SFO_MAX
	    || lr_file_read((u64)s_lba * 2048, d, s_len))
	{
		snprintf(why, wn, "not a PS3 game image (no PS3_GAME/PARAM.SFO in it)");
		goto fail;
	}
	tid[0] = 0;
	sfo_get(d, (int)s_len, "TITLE_ID", tid, sizeof tid);
	say("  a PS3 disc: PARAM.SFO %u bytes, TITLE_ID %s", s_len, tid[0] ? tid : "(none)");

	/* a whole file the system sees too: the same bytes both ways */
	if (whole_sys)
	{
		for (k = 0; k < 10; k++)
		{
			u64 off = k == 0 ? 0 : k == 9 ? (size > CHUNK ? size - CHUNK : 0) : (size / 9 * k) & ~2047ull;
			u32 n = size - off < CHUNK ? (u32)(size - off) : CHUNK;

			if (sys_read(g->path, off, a, n, &got) || got != n || lr_file_read(off, b, n) || memcmp(a, b, n))
			{
				snprintf(why, wn, "the card reader and the system disagree about the file");
				goto fail;
			}
		}
		say("  card reader = system file at 10 places");
	}
	fsport_detach();
	attached = 0;

	/* 2. the sector list; webMAN mounts it */
	sysFsMkdir(DIR_, 0777);
	len = ps3_args(dev, lba, cnt, nr, args, sizeof args);
	if (len < 0 || write_file(NTFS, args, (u32)len))
	{
		snprintf(why, wn, "could not write the sector list");
		goto fail;
	}
	wm_urlenc(enc, sizeof enc, NTFS);
	snprintf(url, sizeof url, "/mount_ps3%s", enc);
	r = wm_get(url, resp, sizeof resp);
	say("  webMAN mount: %d", r);
	if (r != 200)
	{
		snprintf(why, wn, "webMAN did not take the mount (%d)", r);
		goto fail;
	}

	/* 3. /dev_bdvd's PARAM.SFO is the card's */
	for (k = 0; k < 80 && sysFsStat(SFO_BD, &st) != 0; k++)
		usleep(100000);
	got = 0;
	if (k == 80 || (u64)st.st_size != s_len || sys_read(SFO_BD, 0, a, s_len < CHUNK ? s_len : CHUNK, &got)
	    || got != (s_len < CHUNK ? s_len : CHUNK) || memcmp(a, d, got))
	{
		snprintf(why, wn, k == 80 ? "the disc did not appear at /dev_bdvd" : "the disc webMAN made is not the card's game");
		goto fail_mounted;
	}
	say("  /dev_bdvd/PS3_GAME/PARAM.SFO = the card's (%u bytes): ready", got);
	write_file(MOUNTED, tid, (u32)strlen(tid));	/* v0.82: ps3sd_release() at the next start of Prism */
	free(a);
	free(b);
	free(d);
	if (lg)
		fclose(lg);
	lg = NULL;
	return 0;
fail_mounted:
	wm_get("/mount_ps3/unmount", resp, sizeof resp);
fail:
	if (attached)
		fsport_detach();
	say("  not started: %s", why);
	free(a);
	free(b);
	free(d);
	if (lg)
		fclose(lg);
	lg = NULL;
	return -1;
}

/* ---- v0.82 / 1.00: PS1 images on an exFAT volume (a card's or a USB drive's), served by rawseciso in its CD mode
 * (Bananafuzzbuddy 11-27: "just do Ps1 PSP and PS3 exfat usb drives"). A PS1 image on a card's FAT32 part or a FAT32 USB drive is
 * mounted by webMAN the usual way (launch.c mount). The sector list (pspsd_core.c psx_args): the image's runs, its
 * sector size (.iso 2048; .bin / .img 2352, or 2048 when only that divides it), and the tracks of the .cue beside it
 * (one data track when there is none). Before the mount: the image must be a PlayStation disc (ISO 9660 at sector 16,
 * system "PLAYSTATION"). PS1 games never touch /dev_bdvd's files here: the emulator reads the disc itself. */
#define NTFS_PSX	DIR_ "/card.ntfs[PSXISO]"

int ps1sd_card_game(const void *gv)
{
	const game_t *g = (const game_t *)gv;

	return g->exfat && (WHERE_SLOT(g->where) || (g->where >= 3 && g->where <= 10));
}

int ps1sd_prepare(const void *gv, char *why, int wn)
{
	const game_t *g = (const game_t *)gv;
	static uint32_t lba[MAX_RUNS], cnt[MAX_RUNS];
	static u8 args[FI_ARGS_MAX];
	static char cue[16384];
	psx_track_t tr[PSX_MAX_TRACKS];
	u64 dev = game_raw_dev(g), size = 0, csize = 0;
	const char *slash, *dot;
	char dir[16], cpath[208], cuep[220], enc[300], url[340], resp[200];
	u8 pvd[2048];
	int r, nr = 0, nt = 1, len, attached = 0, ss;
	size_t L;

	lg = fopen(LOGF, "a");
	say("=== PS1 game from an exFAT volume, app v%s: %s", APP_VERSION, g->path);
	slash = g->path[0] == '/' ? strchr(g->path + 1, '/') : NULL;
	L = slash ? (size_t)(slash - g->path) : 0;
	if (!dev || !L || L >= sizeof dir)
	{
		snprintf(why, wn, "not on a card or a USB drive Prism reads");
		goto fail;
	}
	memcpy(dir, g->path, L);
	dir[L] = 0;
	snprintf(cpath, sizeof cpath, "%s", g->path + L);
	dot = strrchr(cpath, '.');
	snprintf(cuep, sizeof cuep, "%.*s.cue", dot ? (int)(dot - cpath) : (int)strlen(cpath), cpath);
	tr[0].lba = 0;
	tr[0].audio = 0;

	fsport_log = lg;
	r = fsport_attach(dev);
	say("  device %s (%llx): open %d", dir, (unsigned long long)dev, r);
	attached = r == 0;
	if (r == 0)
	{
		r = lr_fs_mount();
		say("  partitions: %d", r);
	}
	/* the cue first: the card reader has one open file at a time */
	if (r == 0 && lr_fs_open_exfat(cuep, &csize) == 0)	/* 1.00: the exFAT part's own (a FAT32 twin is found first otherwise) */
	{
		if (csize == 0 || csize >= sizeof cue || lr_file_read(0, (u8 *)cue, (u32)csize))
		{
			snprintf(why, wn, "could not read %s", cuep);
			goto fail;
		}
		nt = psx_cue(cue, (int)csize, tr, PSX_MAX_TRACKS);
		say("  %s: %d track(s)", cuep, nt);
		if (nt < 1)
		{
			snprintf(why, wn, "its .cue is not one this reads (one FILE, every TRACK with an INDEX 01)");
			goto fail;
		}
	}
	else if (r == 0)
		say("  no %s: one data track", cuep);
	if (r == 0)
	{
		r = lr_fs_open_exfat(cpath, &size);
		say("  file %s: %d, %llu bytes (exFAT part)", cpath, r, (unsigned long long)size);
	}
	fsport_log = NULL;
	if (r || !size)
	{
		snprintf(why, wn, "the card reader could not open it in %s (%d)", dir, r);
		goto fail;
	}
	ss = 0;
	if ((dot && (dot[1] | 0x20) == 'i' && (dot[2] | 0x20) == 's' && (dot[3] | 0x20) == 'o') ? size % 2048 == 0 : 0)
		ss = 2048;
	else if (size % 2352 == 0)
		ss = 2352;
	else if (size % 2048 == 0)
		ss = 2048;
	if (!ss)
	{
		snprintf(why, wn, "its size is not whole 2352- or 2048-byte sectors");
		goto fail;
	}
	if (lr_file_read((u64)16 * ss + (ss == 2352 ? 24 : 0), pvd, sizeof pvd) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)
	    || memcmp(pvd + 8, "PLAYSTATION", 11))
	{
		snprintf(why, wn, "not a PlayStation disc image (no PLAYSTATION volume at sector 16)");
		goto fail;
	}
	for (nr = 0; nr < MAX_RUNS && lr_fs_run((u32)nr, &lba[nr], &cnt[nr]) > 0; nr++)
		;
	if (nr == 0 || lr_fs_run((u32)nr, &lba[0], &cnt[0]) > 0)
	{
		snprintf(why, wn, "%s", nr ? "the image is in too many pieces" : "no runs");
		goto fail;
	}
	say("  a PlayStation disc, %d-byte sectors, %d track(s), %llu bytes in %d run(s), first at sector %u", ss, nt,
		(unsigned long long)size, nr, lba[0]);
	fsport_detach();
	attached = 0;

	sysFsMkdir(DIR_, 0777);
	len = psx_args(dev, lba, cnt, nr, ss, tr, nt, args, sizeof args);
	if (len < 0 || write_file(NTFS_PSX, args, (u32)len))
	{
		snprintf(why, wn, "could not write the sector list");
		goto fail;
	}
	wm_urlenc(enc, sizeof enc, NTFS_PSX);
	snprintf(url, sizeof url, "/mount_ps3%s", enc);
	r = wm_get(url, resp, sizeof resp);
	say("  webMAN mount: %d", r);
	if (r != 200)
	{
		snprintf(why, wn, "webMAN did not take the mount (%d)", r);
		goto fail;
	}
	say("  mounted: ready");
	if (lg)
		fclose(lg);
	lg = NULL;
	return 0;
fail:
	if (attached)
		fsport_detach();
	fsport_log = NULL;
	say("  not started: %s", why);
	if (lg)
		fclose(lg);
	lg = NULL;
	return -1;
}
