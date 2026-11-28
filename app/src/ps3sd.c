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
int sfo_get(const u8 *d, int n, const char *key, char *out, int out_n);	/* games.c */

#define DIR_	"/dev_hdd0/tmp/prism_ps3"
#define NTFS	DIR_ "/card.ntfs[PS3ISO]"
#define LOGF	"/dev_hdd0/tmp/prism_ps3sd.log"
#define SFO_BD	"/dev_bdvd/PS3_GAME/PARAM.SFO"
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

int ps3sd_card_game(const void *gv)
{
	const game_t *g = (const game_t *)gv;

	return WHERE_SLOT(g->where) && g->kind == 0;
}

int ps3sd_prepare(const void *gv, char *why, int wn)
{
	static const struct { u8 where; const char *dir; u64 dev; } slot[3] = {
		{ 0, "/dev_sd", ST_DEV_SD }, { WHERE_MS, "/dev_ms", ST_DEV_MS }, { WHERE_CF, "/dev_cf", ST_DEV_CF } };
	const game_t *g = (const game_t *)gv;
	static uint32_t lba[MAX_RUNS], cnt[MAX_RUNS];
	static u8 args[FI_ARGS_MAX];
	char cpath[208], enc[300], url[340], resp[200], tid[16];
	u8 *a = NULL, *b = NULL, *d = NULL;
	u32 r_lba, r_len, s_lba = 0, s_len = 0, got = 0;
	sysFSStat st;
	u64 size = 0;
	int s, r, k, nr = 0, len, attached = 0, isdir, whole_sys;
	size_t L;

	lg = fopen(LOGF, "a");
	say("=== PS3 game from the card, app v%s: %s (%s part)", APP_VERSION, g->path, g->exfat ? "exFAT" : "FAT32");
	for (s = 0; s < 3 && slot[s].where != g->where; s++)
		;
	L = s < 3 ? strlen(slot[s].dir) : 0;
	if (s == 3 || strncmp(g->path, slot[s].dir, L) || g->path[L] != '/')
	{
		snprintf(why, wn, "not in a card slot");
		goto fail;
	}
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
	r = fsport_attach(slot[s].dev);
	say("  slot %s: open %d", slot[s].dir, r);
	attached = r == 0;
	if (r == 0)
	{
		r = lr_fs_mount();
		say("  partitions: %d", r);
	}
	if (r == 0)
	{
		r = lr_fs_open(cpath, &size);
		say("  file %s: %d, %llu bytes", cpath, r, (unsigned long long)size);
	}
	fsport_log = NULL;
	if (r || !size || (size & 2047) || (whole_sys && size != (u64)st.st_size))
	{
		snprintf(why, wn, "the card reader could not open it in %s (%s, %d)", slot[s].dir,
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
	len = ps3_args(slot[s].dev, lba, cnt, nr, args, sizeof args);
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
