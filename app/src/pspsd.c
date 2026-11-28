/* v0.60: a PSP game in a card slot played from the card itself, with no copy on the hard disk (Bananafuzzbuddy,
 * 2026-11-24). Why the card cannot simply be mounted, and what webMAN does instead: pspsd_core.h.
 *
 *   1. Prism's card reader (lr_fs.c, read-only) opens the image by the card's raw sectors and lists its runs.
 *   2. Those runs are checked: bytes read through them must equal the same bytes read through the system's
 *      own /dev_sd file, at the start, the end and eight places between. A card reader that disagreed
 *      with the file system would serve the emulator wrong bytes; nothing is mounted then.
 *   3. The fake volume's header and the .ntfs[PSPISO] file go to /dev_hdd0/tmp/prism_psp/, and webMAN
 *      mounts the file: /dev_bdvd becomes a disc holding the image, read from the card's sectors by
 *      webMAN's rawseciso, and Cobra is pointed at /dev_bdvd/<image> (mount_rawiso.h).
 *   4. The image is read back through /dev_bdvd and compared with the card's file once more. Only then
 *      does the PSP launcher start. Any failure: nothing is left mounted and main.c falls back to the copy
 *      on the hard disk (v0.30).
 * Each step goes to /dev_hdd0/tmp/prism_pspsd.log. The card is never written. */
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
#include "pspsd.h"
#include "pspsd_core.h"
#include "version.h"

int lr_fs_run(u32 i, u32 *lba, u32 *n);	/* lr_fs.c, LR_APP */
int lr_fs_open_exfat(const char *path, u64 *size);	/* lr_fs.c, LR_APP (1.00): the exFAT volume(s) only */

#define DIR_	"/dev_hdd0/tmp/prism_psp"
#define HDR	DIR_ "/card.hdr"
#define NTFS	DIR_ "/card.ntfs[PSPISO]"
#define LOGF	"/dev_hdd0/tmp/prism_pspsd.log"
#define CHUNK	(64 * 1024)
#define MAX_RUNS	((FI_ARGS_MAX - 20 - 0x200) / 8 - 1)

static FILE *lg;
static char mounted[160];	/* /dev_bdvd/<a Joliet name of up to 110 bytes> */

static int ends_iso(const char *s)
{
	size_t n = strlen(s);

	return n > 4 && s[n - 4] == '.' && (s[n - 3] | 0x20) == 'i' && (s[n - 2] | 0x20) == 's' && (s[n - 1] | 0x20) == 'o';
}

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

/* read len bytes at off of a file through the system */
static int sys_read(const char *path, u64 off, u8 *buf, u32 len)
{
	s32 fd;
	u64 got = 0, pos;
	int r = -1;

	if (sysFsOpen(path, SYS_O_RDONLY, &fd, NULL, 0) != 0)
		return -1;
	if (sysFsLseek(fd, (s64)off, 0, &pos) == 0 && sysFsRead(fd, buf, len, &got) == 0 && got == len)
		r = 0;
	sysFsClose(fd);
	return r;
}

/* the same bytes, through the card reader (2) and through the system (1), at 10 places */
static int same_bytes(const char *sys_path, u64 size, u8 *a, u8 *b, int via_card)
{
	int k;

	for (k = 0; k < 10; k++)
	{
		u64 off = k == 0 ? 0 : k == 9 ? (size > CHUNK ? size - CHUNK : 0) : (size / 9 * k) & ~2047ull;
		u32 len = size - off < CHUNK ? (u32)(size - off) : CHUNK;
		int r1, r2;

		r1 = sys_read(sys_path, off, a, len);
		r2 = via_card ? lr_file_read(off, b, len) : sys_read(mounted, off, b, len);
		if (r1 || r2 || memcmp(a, b, len))
		{
			say("  bytes at %llx (%u): %s", (unsigned long long)off, len, r1 ? "the system's read failed" : r2 ? "the second read failed" : "DIFFER");
			return -1;
		}
	}
	return 0;
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

static void unmount(void)
{
	char resp[200];

	wm_get("/mount_ps3/unmount", resp, sizeof resp);
	mounted[0] = 0;
}

/* v0.82: the k-th of the 10 places same_bytes() looks at, and a fingerprint of the bytes there */
static void place(int k, u64 size, u64 *off, u32 *len)
{
	*off = k == 0 ? 0 : k == 9 ? (size > CHUNK ? size - CHUNK : 0) : (size / 9 * k) & ~2047ull;
	*len = size - *off < CHUNK ? (u32)(size - *off) : CHUNK;
}

static u64 fnv64(const u8 *p, u32 n)
{
	u64 h = 0xCBF29CE484222325ull;

	while (n--)
		h = (h ^ *p++) * 0x100000001B3ull;
	return h;
}

int pspsd_mounted(const char *path)
{
	return mounted[0] && !strcmp(path, NTFS);
}

int pspsd_prepare(const void *gv, char *path_out, int n, char *why, int wn)
{
	const game_t *g = (const game_t *)gv;
	const char *leaf = strrchr(g->path, '/'), *slash;
	u64 dev = game_raw_dev(g), want[10];	/* v0.82: a card slot or a USB port (games.c); exFAT fingerprints */
	char dir[16];
	size_t L;
	static uint32_t lba[MAX_RUNS], cnt[MAX_RUNS];
	static u8 hdr[FI_HDR_SIZE], args[FI_ARGS_MAX];
	u8 *a = NULL, *b = NULL;
	sysFSStat st;
	u64 size = 0;
	int r, k, nr = 0, len, attached = 0;
	char enc[300], url[340], resp[200];

	mounted[0] = 0;
	lg = fopen(LOGF, "a");
	say("=== PSP game from the card, app v%s: %s", APP_VERSION, g->path);
	/* v0.82: the device's GameOS name is the path's first part ("/dev_sd", "/dev_usb003": an exFAT USB drive's) */
	slash = g->path[0] == '/' ? strchr(g->path + 1, '/') : NULL;
	L = slash ? (size_t)(slash - g->path) : 0;
	if (!dev || !L || L >= sizeof dir || !leaf)
	{
		snprintf(why, wn, "not on a card or a USB drive Prism reads");
		goto fail;
	}
	memcpy(dir, g->path, L);
	dir[L] = 0;
	if (!ends_iso(leaf + 1))
	{
		snprintf(why, wn, "only a plain .iso can be served this way");
		goto fail;
	}
	if (!g->exfat && (sysFsStat(g->path, &st) != 0 || !st.st_size))
	{
		snprintf(why, wn, "the system cannot see the file");
		goto fail;
	}
	a = (u8 *)malloc(CHUNK);
	b = (u8 *)malloc(CHUNK);
	if (!a || !b)
	{
		snprintf(why, wn, "out of memory");
		goto fail;
	}
	/* 1. the image's runs, by the card's raw sectors. v0.64: each step logged, and the card reader's own lines (lr_log)
	 * go to this log too (Bananafuzzbuddy 11-24: from the CF slot it stopped here with -5 and no reason) */
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
		r = g->exfat ? lr_fs_open_exfat(g->path + L, &size) : lr_fs_open(g->path + L, &size);	/* 1.00: see ps3sd.c */
		say("  file %s: %d, %llu bytes", g->path + L, r, (unsigned long long)size);
	}
	fsport_log = NULL;
	if (r || !size || (!g->exfat && size != (u64)st.st_size))
	{
		snprintf(why, wn, "the card reader could not open it in %s (%s, %d)", dir,
			!attached ? "the slot" : r ? "the partitions or the file" : "a different size", r);
		goto fail;
	}
	for (nr = 0; nr < MAX_RUNS && lr_fs_run((u32)nr, &lba[nr], &cnt[nr]) > 0; nr++)
		;
	if (nr == 0 || lr_fs_run((u32)nr, &lba[0], &cnt[0]) > 0)
	{
		snprintf(why, wn, "%s", nr ? "the image is in too many pieces on the card" : "no runs");
		goto fail;
	}
	say("  %llu bytes in %d run(s), first at card sector %u", (unsigned long long)size, nr, lba[0]);
	/* 2. the runs give the system's bytes. v0.82: on an exFAT volume there is no system file: the image must be a PSP
	 * disc (ISO 9660 at sector 16, system "PSP GAME"), and 10 places are fingerprinted through the card reader now and
	 * compared through /dev_bdvd after the mount (never both at once) */
	if (!g->exfat)
	{
		if (same_bytes(g->path, size, a, b, 1))
		{
			snprintf(why, wn, "the card reader and the system disagree about the file");
			goto fail;
		}
		say("  card reader = system file at 10 places");
	}
	else
	{
		if (lr_file_read(16 * 2048, a, 2048) || a[0] != 1 || memcmp(a + 1, "CD001", 5) || memcmp(a + 8, "PSP GAME", 8))
		{
			snprintf(why, wn, "not a PSP disc image (no PSP GAME volume at sector 16)");
			goto fail;
		}
		for (k = 0; k < 10; k++)
		{
			u64 off;
			u32 n2;

			place(k, size, &off, &n2);
			if (lr_file_read(off, b, n2))
			{
				snprintf(why, wn, "the card reader could not read the image at %llu", (unsigned long long)off);
				goto fail;
			}
			want[k] = fnv64(b, n2);
		}
		say("  a PSP disc on an exFAT volume: 10 places read through the card reader");
	}
	fsport_detach();
	attached = 0;
	/* 3. the header and the .ntfs file; webMAN mounts it */
	sysFsMkdir(DIR_, 0777);
	if (fi_header(leaf + 1, size, hdr) || write_file(HDR, hdr, FI_HDR_SIZE))
	{
		snprintf(why, wn, "could not write the disc header");
		goto fail;
	}
	len = fi_args(dev, HDR, lba, cnt, nr, args, sizeof args);
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
	/* 4. the image as the emulator will see it: /dev_bdvd/<its name>, the same bytes as the card's file */
	{
		s32 fd;
		u64 rd;
		sysFSDirent e;

		mounted[0] = 0;
		for (k = 0; k < 50 && !mounted[0]; k++)
		{
			if (sysFsOpendir("/dev_bdvd", &fd) == 0)
			{
				while (sysFsReaddir(fd, &e, &rd) == 0 && rd > 0)
					if (e.d_name[0] != '.')
					{
						snprintf(mounted, sizeof mounted, "/dev_bdvd/%.140s", e.d_name);
						break;
					}
				sysFsClosedir(fd);
			}
			if (!mounted[0])
				usleep(100000);
		}
		if (!mounted[0] || sysFsStat(mounted, &st) != 0 || (u64)st.st_size != size)
		{
			snprintf(why, wn, "the disc webMAN made does not hold the image");
			goto fail_mounted;
		}
		if (!g->exfat && same_bytes(g->path, size, a, b, 0))
		{
			snprintf(why, wn, "the disc webMAN made gives other bytes than the card");
			goto fail_mounted;
		}
		for (k = 0; g->exfat && k < 10; k++)
		{
			u64 off;
			u32 n2;

			place(k, size, &off, &n2);
			if (sys_read(mounted, off, b, n2) || fnv64(b, n2) != want[k])
			{
				snprintf(why, wn, "the disc webMAN made gives other bytes than the card (at %llu)", (unsigned long long)off);
				goto fail_mounted;
			}
		}
		say("  %s = the card's file at 10 places: ready", mounted);
	}
	snprintf(path_out, n, "%s", NTFS);
	strcpy(mounted, "x");	/* pspsd_mounted(): launch.c need not mount it again */
	free(a);
	free(b);
	if (lg)
		fclose(lg);
	lg = NULL;
	return 0;
fail_mounted:
	unmount();
fail:
	if (attached)
		fsport_detach();
	mounted[0] = 0;
	say("  not from the card: %s", why);
	free(a);
	free(b);
	if (lg)
		fclose(lg);
	lg = NULL;
	return -1;
}
