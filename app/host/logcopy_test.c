/* v0.81 PC test of src/logcopy.c (Settings' "Write logs, copy to USB"), with src/dirlist.c, both unchanged.
 * The console's file calls are faked over a scratch folder (FAKE_ROOT + the console path).
 *   bash host/logcopy_test.sh
 * Checks: what is copied (prism_*.log / *.cfg and onxmb.bat, byte for byte), what is not (other names, a file over
 * 4 MB, folders), the README, the message, no USB drive, a USB drive in a later slot, a hard disk without the files.
 * Calibrated: a copy changed after the call, and a README missing a file, are both caught (the "known-bad" runs). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <lv2/sysfs.h>
#include "check.h"
#include "logcopy.h"

static char root[512];
static int fails, checks;

static void path_of(char *out, size_t n, const char *p)
{
	snprintf(out, n, "%s%s", root, p);
}

/* ---- the fakes */
check_t chk;

const char *model_text(int code)
{
	return code == 1 ? "CECHA / CECHB, COK-001" : "CECHC / CECHE, COK-002";
}

FILE *fake_fopen(const char *p, const char *mode)
{
	char q[1024];

	path_of(q, sizeof q, p);
	return fopen(q, mode);
}

s32 sysFsOpen(const char *p, s32 fl, s32 *fd, const void *a, u64 n)
{
	char q[1024];
	int f = (fl & 3) == SYS_O_RDONLY ? O_RDONLY : (fl & 3) == SYS_O_WRONLY ? O_WRONLY : O_RDWR;

	(void)a; (void)n;
	if (fl & SYS_O_CREAT) f |= O_CREAT;
	if (fl & SYS_O_TRUNC) f |= O_TRUNC;
	path_of(q, sizeof q, p);
	*fd = open(q, f, 0666);
	return *fd < 0 ? -1 : 0;
}

s32 sysFsRead(s32 fd, void *b, u64 len, u64 *got)
{
	ssize_t r = read(fd, b, len);

	*got = r > 0 ? (u64)r : 0;
	return r < 0 ? -1 : 0;
}

s32 sysFsWrite(s32 fd, const void *b, u64 len, u64 *w)
{
	ssize_t r = write(fd, b, len);

	*w = r > 0 ? (u64)r : 0;
	return r < 0 ? -1 : 0;
}

s32 sysFsClose(s32 fd) { return close(fd) ? -1 : 0; }

s32 sysFsStat(const char *p, sysFSStat *st)
{
	char q[1024];
	struct stat s;

	path_of(q, sizeof q, p);
	if (stat(q, &s))
		return -1;
	memset(st, 0, sizeof *st);
	st->st_mode = (s32)s.st_mode;
	st->st_size = (u64)s.st_size;
	return 0;
}

s32 sysFsMkdir(const char *p, s32 m)
{
	char q[1024];

	path_of(q, sizeof q, p);
	return mkdir(q, (mode_t)m) ? -1 : 0;
}

s32 sysFsChmod(const char *p, s32 m) { (void)p; (void)m; return 0; }

static DIR *dirs[8];
static char dirpath[8][1024];

s32 sysFsOpendir(const char *p, s32 *fd)
{
	int i;

	for (i = 0; i < 8 && dirs[i]; i++)
		;
	if (i == 8)
		return -1;
	path_of(dirpath[i], sizeof dirpath[i], p);
	dirs[i] = opendir(dirpath[i]);
	if (!dirs[i])
		return -1;
	*fd = i;
	return 0;
}

s32 sysFsGetDirectoryEntries(s32 fd, sysFSDirectoryEntry *e, u32 size, u32 *count)
{
	u32 n = 0, max = size / sizeof *e;
	struct dirent *d;

	while (n < max && (d = readdir(dirs[fd])))
	{
		char q[2048];
		struct stat s;

		memset(&e[n], 0, sizeof e[n]);
		strncpy(e[n].entry_name.d_name, d->d_name, 255);
		e[n].entry_name.d_namlen = (u8)strlen(e[n].entry_name.d_name);
		snprintf(q, sizeof q, "%s/%s", dirpath[fd], d->d_name);
		if (!stat(q, &s))
		{
			e[n].attribute.st_mode = (s32)s.st_mode;
			e[n].attribute.st_size = (u64)s.st_size;
			e[n].entry_name.d_type = S_ISDIR(s.st_mode) ? 1 : 2;
		}
		n++;
	}
	*count = n;
	return 0;
}

s32 sysFsClosedir(s32 fd) { closedir(dirs[fd]); dirs[fd] = NULL; return 0; }

/* ---- the test */
static void ok(const char *what, int good, const char *detail)
{
	checks++;
	if (!good)
		fails++;
	printf("%s %s%s%s\n", good ? "PASS" : "FAIL", what, good ? "" : ": ", good ? "" : detail);
}

static void mk(const char *p)
{
	char q[1024];

	path_of(q, sizeof q, p);
	mkdir(q, 0777);
}

static void put(const char *p, unsigned long n, int seed)
{
	char q[1024];
	FILE *f;
	unsigned long i;

	path_of(q, sizeof q, p);
	f = fopen(q, "wb");
	for (i = 0; i < n; i++)
		fputc((int)((i * 31 + (unsigned long)seed) & 0xFF), f);
	fclose(f);
}

static int same(const char *a, const char *b)
{
	char qa[1024], qb[1024];
	FILE *fa, *fb;
	int ca, cb;

	path_of(qa, sizeof qa, a);
	path_of(qb, sizeof qb, b);
	fa = fopen(qa, "rb");
	fb = fopen(qb, "rb");
	if (!fa || !fb)
	{
		if (fa) fclose(fa);
		if (fb) fclose(fb);
		return 0;
	}
	do
	{
		ca = fgetc(fa);
		cb = fgetc(fb);
	} while (ca == cb && ca != EOF);
	fclose(fa);
	fclose(fb);
	return ca == cb;
}

static int there(const char *p)
{
	char q[1024];
	struct stat s;

	path_of(q, sizeof q, p);
	return stat(q, &s) == 0;
}

static char *slurp(const char *p)
{
	static char buf[65536];
	char q[1024];
	FILE *f;
	size_t n;

	path_of(q, sizeof q, p);
	buf[0] = 0;
	f = fopen(q, "rb");
	if (!f)
		return buf;
	n = fread(buf, 1, sizeof buf - 1, f);
	buf[n] = 0;
	fclose(f);
	return buf;
}

static void fresh(const char *name)
{
	char cmd[1200];

	snprintf(root, sizeof root, "%s/%s", getenv("LOGCOPY_ROOT") ? getenv("LOGCOPY_ROOT") : "/tmp/logcopy_test", name);
	snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", root, root);
	if (system(cmd)) exit(2);
	mk("/dev_hdd0");
	mk("/dev_hdd0/tmp");
}

int main(int argc, char **argv)
{
	static const char *const copied[] = { "prism_a01.log", "prism_check.log", "prism_app.log", "prism_a01.cfg", "prism_lr.log" };
	static const char *const not_copied[] = { "prism_art.dat", "other.log", "prism_psp_launch.txt", "wm_request" };
	char msg[80], a[300], b[300], *rd;
	int r, i, bad = argc > 1 ? atoi(argv[1]) : 0;	/* 1, 2: the known-bad runs */

	memset(&chk, 0, sizeof chk);
	chk.model = 1; chk.a01 = 1; chk.emu = 0; chk.cobra = 1; chk.cobra_bcd = 0x0850; chk.wm = 1;
	strcpy(chk.fw, "4.93 CEX");
	strcpy(chk.why, "");
	chk.ps2_ok = 1;

	/* 1. the CECHA tester's console: a USB drive in slot 0 */
	fresh("one");
	mk("/dev_usb000");
	put("/dev_hdd0/tmp/prism_a01.log", 0x3000, 1);
	put("/dev_hdd0/tmp/prism_check.log", 412, 2);
	put("/dev_hdd0/tmp/prism_app.log", 2900, 3);
	put("/dev_hdd0/tmp/prism_a01.cfg", 88, 4);
	put("/dev_hdd0/tmp/prism_lr.log", 70000, 5);		/* larger than one 64 KB chunk */
	put("/dev_hdd0/tmp/prism_big.log", (4ul << 20) + 1, 6);	/* over 4 MB: listed, not copied */
	put("/dev_hdd0/tmp/prism_art.dat", 5000, 7);
	put("/dev_hdd0/tmp/other.log", 10, 8);
	put("/dev_hdd0/tmp/prism_psp_launch.txt", 10, 9);
	put("/dev_hdd0/tmp/wm_request", 10, 10);
	mk("/dev_hdd0/tmp/prism_vshplug");			/* a folder named like a log: never copied */
	put("/dev_hdd0/onxmb.bat", 376, 11);
	r = logs_to_usb(msg, sizeof msg);
	if (bad == 1)
		put("/dev_usb000/PRISM_LOGS/prism_check.log", 412, 99);	/* known-bad: a copy that is not the original */
	ok("returns the count: 5 logs + onxmb.bat", r == 6, msg);
	ok("the message names the drive and the folder", !strcmp(msg, "6 logs copied to USB 0, folder PRISM_LOGS"), msg);
	for (i = 0; i < 5; i++)
	{
		snprintf(a, sizeof a, "/dev_hdd0/tmp/%s", copied[i]);
		snprintf(b, sizeof b, "/dev_usb000/PRISM_LOGS/%s", copied[i]);
		ok(copied[i], same(a, b), "missing or not byte for byte");
	}
	ok("onxmb.bat as onxmb.bat.txt", same("/dev_hdd0/onxmb.bat", "/dev_usb000/PRISM_LOGS/onxmb.bat.txt"), "missing or different");
	for (i = 0; i < 4; i++)
	{
		snprintf(b, sizeof b, "/dev_usb000/PRISM_LOGS/%s", not_copied[i]);
		ok(not_copied[i], !there(b), "copied, and must not be");
	}
	ok("the 4 MB + 1 log is not copied", !there("/dev_usb000/PRISM_LOGS/prism_big.log"), "copied");
	ok("the folder prism_vshplug is not copied", !there("/dev_usb000/PRISM_LOGS/prism_vshplug"), "copied");
	rd = slurp("/dev_usb000/PRISM_LOGS/README.txt");
	if (bad == 2)
		*strstr(rd, "prism_a01.cfg") = 'X';			/* known-bad: a README that does not list a file */
	ok("README: version, model, a different emulator", strstr(rd, "Prism 0.81") && strstr(rd, "CECHA / CECHB, COK-001 (sub-code 1)")
		&& strstr(rd, "a DIFFERENT one") && strstr(rd, "4.93 CEX") && strstr(rd, "cobra      8.5"), rd);
	ok("README lists every copied file and the one too big", strstr(rd, "prism_a01.log") && strstr(rd, "prism_a01.cfg") && strstr(rd, "prism_lr.log")
		&& strstr(rd, "onxmb.bat (as onxmb.bat.txt)") && strstr(rd, "prism_big.log") && strstr(rd, "too big: not copied")
		&& !strstr(rd, "COPY FAILED") && !strstr(rd, "other.log"), rd);
	ok("nothing written on the hard disk", !there("/dev_hdd0/tmp/README.txt") && !there("/dev_hdd0/PRISM_LOGS"), "written");

	/* 2. a second press: the same folder, the files overwritten */
	put("/dev_hdd0/tmp/prism_a01.log", 0x3000, 21);
	r = logs_to_usb(msg, sizeof msg);
	ok("a second press overwrites with the newer log", r == 6 && same("/dev_hdd0/tmp/prism_a01.log", "/dev_usb000/PRISM_LOGS/prism_a01.log"), msg);

	/* 3. no USB drive */
	fresh("nousb");
	put("/dev_hdd0/tmp/prism_a01.log", 100, 1);
	r = logs_to_usb(msg, sizeof msg);
	ok("no USB drive: -1 and says so", r == -1 && !strcmp(msg, "Logs written. No USB drive found: put one in, try again"), msg);

	/* 4. the drive in slot 2 only */
	fresh("slot2");
	mk("/dev_usb002");
	put("/dev_hdd0/tmp/prism_a01.log", 100, 1);
	r = logs_to_usb(msg, sizeof msg);
	ok("a drive in slot 2 is used", r == 1 && same("/dev_hdd0/tmp/prism_a01.log", "/dev_usb002/PRISM_LOGS/prism_a01.log")
		&& !strcmp(msg, "1 logs copied to USB 2, folder PRISM_LOGS"), msg);

	/* 5. no logs at all, no onxmb.bat: the README alone, 0 */
	fresh("empty");
	mk("/dev_usb000");
	r = logs_to_usb(msg, sizeof msg);
	ok("no logs: 0 copied, the README still written", r == 0 && there("/dev_usb000/PRISM_LOGS/README.txt"), msg);

	/* 6. the folder cannot be made (a file of that name is in the way) */
	fresh("blocked");
	mk("/dev_usb000");
	put("/dev_usb000/PRISM_LOGS", 1, 1);
	put("/dev_hdd0/tmp/prism_a01.log", 100, 1);
	r = logs_to_usb(msg, sizeof msg);
	ok("no folder: -2 and says so (a file named PRISM_LOGS)", r < 0, msg);

	printf("logcopy: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
