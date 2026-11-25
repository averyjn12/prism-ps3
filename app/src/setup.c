/* The one-time setup for PS2 games from the SD card (v0.15, for the public release).
 *
 * PS2 games from the card need Prism's helper (our build of Cobra's PS2 stage2, lr_g5) to be
 * what Cobra copies into the PS2 emulator at each PS2 start. Cobra copies it from
 * /dev_flash/ps2emu/ps2gxemu_stage2.bin; a webMAN "map" of that path to our file makes Cobra copy
 * ours instead, and lives in kernel RAM only (nothing is ever written to flash). webMAN runs
 * /dev_hdd0/onxmb.bat each time the XMB comes up (poll.h: on the way back from any game, and at
 * boot), so the setup is these lines at the END of that file:
 *
 *   # PRISM-APP-SETUP begin ...
 *   if exist /dev_hdd0/tmp/prism_stage2.bin
 *   map /dev_flash/ps2emu/ps2gxemu_stage2.bin=/dev_hdd0/tmp/prism_stage2.bin
 *   del /dev_hdd0/tmp/prism_setup_pending
 *   end if
 *   # PRISM-APP-SETUP end
 *
 * plus two files the app carries in USRDIR/setup: the helper (-> /dev_hdd0/tmp) and the launcher
 * image every PS2 start from the app mounts (-> /dev_hdd0/PS2ISO). The "del" line is how the app
 * knows the lines have run. At the end, not the start: webMAN's parser jumps to the next "else" /
 * "end if" in the WHOLE file when a test fails, so a block in front of someone else's lines could
 * land in theirs (webMAN-MOD include/feat/script.h); and the comments avoid those words.
 * Whatever else is in onxmb.bat stays as it was; the file as it was before is kept in
 * /dev_hdd0/tmp/prism_onxmb_backup.bat.
 *
 * Removing takes our lines out and deletes the launcher image. The helper file stays until the
 * console has restarted (the map in force until then still points at it), then the app deletes it.
 * Lines Prism did not write (the bench's PRISM-LR-G6 script) are recognised and left alone. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ppu-lv2.h>
#include <lv2/sysfs.h>
#include <sys/systime.h>
#include <sys/file.h>
#include "setup.h"
#include "check.h"
#include "webman.h"

#define APP_SETUP	"/dev_hdd0/game/PRSM00001/USRDIR/setup"
/* v0.54: on a CECHA / CECHB (chk.a01) the A01 helper (lr/out/lr_hw5k11.bin, built for ps2_emu) is
 * installed under its own name and loaded through ps2_emu's stage2 slot; the E01 lines are as before */
#define SRC_STAGE2	(chk.a01 ? APP_SETUP "/prism_stage2_a01.bin" : APP_SETUP "/prism_stage2.bin")
#define SRC_CARRIER	APP_SETUP "/carrier.iso"
#define SRC_COVER	APP_SETUP "/carrier.png"
#define SRC_TT		APP_SETUP "/tinytees.iso"
#define DST_TT		"/dev_hdd0/PS2ISO/Tiny Tees.iso"
#define TT_DONE		"/dev_hdd0/game/PRSM00001/USRDIR/tinytees.done"
#define DST_STAGE2	(chk.a01 ? "/dev_hdd0/tmp/prism_stage2_a01.bin" : "/dev_hdd0/tmp/prism_stage2.bin")
#define A01_LOG		"/dev_hdd0/tmp/prism_a01.log"	/* the A01 helper's log (lr.h): written only if it exists */
#define A01_KIT_HELPER	"prism_a01_stage2.bin"		/* the Prism A01 Test kit's own map target */
#define PS2ISO_DIR	"/dev_hdd0/PS2ISO"
#define DST_CARRIER	PS2ISO_DIR "/Prism Launcher.iso"
#define DST_COVER	PS2ISO_DIR "/Prism Launcher.png"
#define ONXMB		"/dev_hdd0/onxmb.bat"
#define ONXMB_NEW	"/dev_hdd0/onxmb.bat.prism_new"
#define BACKUP		"/dev_hdd0/tmp/prism_onxmb_backup.bat"
#define PENDING		"/dev_hdd0/tmp/prism_setup_pending"
#define REMOVED		"/dev_hdd0/tmp/prism_setup_removed"
#define MAP_LINE	(chk.a01 ? "map /dev_flash/ps2emu/ps2hwemu_stage2.bin=" : "map /dev_flash/ps2emu/ps2gxemu_stage2.bin=")
#define MARK_BEGIN	"# PRISM-APP-SETUP begin"
#define MARK_END	"# PRISM-APP-SETUP end"
#define NO_FILE_NOTE	"# (there was no onxmb.bat before Prism's setup)\n"
#define BAT_MAX		(60 * 1024)
#define BLOCK_MAX	1024

/* v0.54: made at run time (the paths depend on the model); on a CECHC / CECHE the text is the same,
 * byte for byte, as the v0.15 - v0.53 constant */
static const char *block_text(void)
{
	static char b[BLOCK_MAX];

	snprintf(b, sizeof b, "%s: the Prism app (PRSM00001) wrote these lines; its SETTINGS page takes them out.\n"
		"# They load Prism's PS2 helper for PS2 games from the SD card. Nothing is written to flash.\n"
		"if exist %s\n%s%s\ndel %s\nend if\n%s\n", MARK_BEGIN, DST_STAGE2, MAP_LINE, DST_STAGE2, PENDING, MARK_END);
	return b;
}

static const char *map_full(void)	/* MAP_LINE DST_STAGE2 */
{
	static char b[160];

	snprintf(b, sizeof b, "%s%s", MAP_LINE, DST_STAGE2);
	return b;
}

setup_t su;

static void wm_shortcuts_remove(void);	/* v0.55, below */

/* ---- files ---- */

static int read_all(const char *path, char *buf, int max)
{
	s32 fd;
	u64 n = 0;

	if (sysFsOpen(path, SYS_O_RDONLY, &fd, NULL, 0) != 0)
		return -1;
	if (sysFsRead(fd, buf, (u64)max, &n) != 0)
		n = 0;
	sysFsClose(fd);
	return (int)n;
}

static int write_all(const char *path, const void *buf, int n)
{
	s32 fd;
	u64 w = 0;
	int r;

	if (sysFsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, NULL, 0) != 0)
		return -1;
	r = (n == 0 || (sysFsWrite(fd, buf, (u64)n, &w) == 0 && w == (u64)n)) ? 0 : -2;
	if (sysFsClose(fd) != 0)
		r = -3;
	sysFsChmod(path, 0666);
	return r;
}

static int exists(const char *p)
{
	sysFSStat st;

	return sysFsStat(p, &st) == 0;
}

/* 1 = same bytes, 0 = different, -1 = one of them missing */
static int same_file(const char *a, const char *b)
{
	static u8 ba[64 * 1024], bb[64 * 1024];
	s32 fa, fb;
	sysFSStat sa, sb;
	int r = 1;

	if (sysFsStat(a, &sa) != 0 || sysFsStat(b, &sb) != 0)
		return -1;
	if (sa.st_size != sb.st_size)
		return 0;
	if (sysFsOpen(a, SYS_O_RDONLY, &fa, NULL, 0) != 0)
		return -1;
	if (sysFsOpen(b, SYS_O_RDONLY, &fb, NULL, 0) != 0)
	{
		sysFsClose(fa);
		return -1;
	}
	for (;;)
	{
		u64 na = 0, nb = 0;

		if (sysFsRead(fa, ba, sizeof ba, &na) != 0 || sysFsRead(fb, bb, sizeof bb, &nb) != 0 || na != nb)
		{
			r = 0;
			break;
		}
		if (na == 0)
			break;
		if (memcmp(ba, bb, (size_t)na))
		{
			r = 0;
			break;
		}
	}
	sysFsClose(fa);
	sysFsClose(fb);
	return r;
}

static int copy_verify(const char *src, const char *dst)
{
	static u8 b[256 * 1024];
	s32 fi, fo;
	int r = 0;

	if (sysFsOpen(src, SYS_O_RDONLY, &fi, NULL, 0) != 0)
		return -1;
	if (sysFsOpen(dst, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fo, NULL, 0) != 0)
	{
		sysFsClose(fi);
		return -2;
	}
	for (;;)
	{
		u64 n = 0, w = 0;

		if (sysFsRead(fi, b, sizeof b, &n) != 0)
		{
			r = -3;
			break;
		}
		if (n == 0)
			break;
		if (sysFsWrite(fo, b, n, &w) != 0 || w != n)
		{
			r = -4;
			break;
		}
	}
	sysFsClose(fi);
	if (sysFsClose(fo) != 0 && !r)
		r = -5;
	sysFsChmod(dst, 0666);
	if (!r && same_file(src, dst) != 1)
		r = -6;
	return r;
}

/* ---- v0.28 / v0.29: "Start Prism with the console" (SETTINGS) ----
 * v0.28 put the whole start in /dev_hdd0/autoexec.bat (wait for the sign-in, then webMAN's
 * /play.ps3?PRSM00001), and Prism did not start (Bananafuzzbuddy, 11-23). Two reasons, in webMAN-MOD's source:
 *  - /play.ps3?<title ID> presses the XMB's /app_home icon by moving the cursor (vsh.h
 *    exec_xmb_item); webMAN's author: when the timing is off "the focus could be in a different
 *    icon". Sent from the PC on 11-23 it started Prism once and opened a PS2 game the next time.
 *  - a script that waits holds webMAN's script lock (script.h parse_script: script_running), and any
 *    other script that starts meanwhile is skipped - onxmb.bat too, where Prism's PS2 lines are.
 * v0.29, three pieces, none of which waits while it holds the lock at start-up:
 *  - autoexec.bat (once per start, at once): copies a marker file into /dev_hdd0/tmp;
 *  - onxmb.bat (each time the XMB comes up): with the marker there, calls USRDIR/autostart.bat;
 *  - autostart.bat (in the package): deletes the marker, waits for the sign-in, mounts Prism's boot
 *    disc (USRDIR/boot, a PS3 disc folder) and presses the disc icon - the press that has started
 *    PS3 games since v0.17, because the XMB moves to a newly inserted disc by itself. The boot
 *    disc's program (boot/boot.c) asks the system to start Prism (cellGameExecGame) and ends;
 *    Prism unmounts the disc (autostart_started).
 * The boot disc is mounted from a copy in /dev_hdd0/tmp/prism_boot: webMAN mounts anything under
 * /dev_hdd0/game/ as an app on /app_home and never as a disc (mount_misc.h; seen on the console
 * 11-23: /dev_bdvd stayed absent). Turning the switch on, and each start of Prism while it is on,
 * brings the copy up to date (six small files, each compared, copied and read back).
 * Each file gets one block at its END between the markers and loses exactly that block again; each
 * is backed up first (once), written beside itself, swapped in and read back. The comments avoid
 * the words webMAN's skip-to-"end if" search looks for. */
#define AUTOEXEC	"/dev_hdd0/autoexec.bat"
#define AUTOEXEC_NEW	"/dev_hdd0/autoexec.bat.prism_new"
#define AUTOEXEC_BACKUP	"/dev_hdd0/tmp/prism_autoexec_backup.bat"
#define ONXMB_BACKUP2	"/dev_hdd0/tmp/prism_onxmb_backup_autostart.bat"
#define AUTO_MARK	"/dev_hdd0/tmp/prism_autostart"
#define AUTO_BAT	"/dev_hdd0/game/PRSM00001/USRDIR/autostart.bat"
#define BOOT_DISC_MARK	"/dev_bdvd/PRISM_BOOT.TXT"
#define BOOT_SRC	"/dev_hdd0/game/PRSM00001/USRDIR/boot"
#define BOOT_DST	"/dev_hdd0/tmp/prism_boot"	/* autostart.bat mounts this path */
#define AUTO_BEGIN	"# PRISM-AUTOSTART begin"
#define AUTO_END	"# PRISM-AUTOSTART end"
#define AUTO_NOFILE_A	"# (there was no autoexec.bat before Prism)\n"
#define AUTO_NOFILE_X	"# (there was no onxmb.bat before Prism's start-up lines)\n"
#define AUTO_LOG	"/dev_hdd0/tmp/prism_autostart.log"

static const char auto_exec_block[] =
	AUTO_BEGIN ": the Prism app (PRSM00001) wrote these lines; its SETTINGS page takes them out.\n"
	"# v0.29: a marker for Prism's lines in onxmb.bat, so that Prism opens once after each start.\n"
	"copy /dev_hdd0/game/PRSM00001/PARAM.SFO=" AUTO_MARK "\n"
	AUTO_END "\n";

static const char auto_xmb_block[] =
	AUTO_BEGIN ": the Prism app (PRSM00001) wrote these lines; its SETTINGS page takes them out.\n"
	"# v0.29: the first time the XMB comes up after a start, open Prism (USRDIR/autostart.bat).\n"
	"if exist " AUTO_MARK "\n"
	"call " AUTO_BAT "\n"
	"end if\n"
	AUTO_END "\n";

static void auto_log(const char *what, int r)
{
	FILE *f = fopen(AUTO_LOG, "a");

	if (f)
	{
		fprintf(f, "%s: %d\n", what, r);
		fclose(f);
	}
}

static int swap_in(const char *path, const char *tmp, const char *text)
{
	static char back[BAT_MAX + 1];
	int n = (int)strlen(text), m;

	if (write_all(tmp, text, n))
		return -1;
	m = read_all(tmp, back, BAT_MAX);
	if (m != n || memcmp(back, text, n))
	{
		sysFsUnlink(tmp);
		return -2;
	}
	sysFsUnlink(path);
	if (sysLv2FsRename(tmp, path) != 0)
		return -3;
	m = read_all(path, back, BAT_MAX);
	return (m == n && !memcmp(back, text, n)) ? 0 : -4;
}

static const char *const boot_files[] = {
	"PRISM_BOOT.TXT", "PS3_DISC.SFB", "PS3_GAME/PARAM.SFO", "PS3_GAME/ICON0.PNG",
	"PS3_GAME/LICDIR/LIC.DAT", "PS3_GAME/USRDIR/EBOOT.BIN"
};
static const char *const boot_dirs[] = {
	BOOT_DST, BOOT_DST "/PS3_GAME", BOOT_DST "/PS3_GAME/LICDIR", BOOT_DST "/PS3_GAME/USRDIR"
};
#define N_BOOT_FILES	((int)(sizeof boot_files / sizeof *boot_files))
#define N_BOOT_DIRS	((int)(sizeof boot_dirs / sizeof *boot_dirs))

/* the boot disc copied out of Prism's folder; 0 = the copy matches the package's files */
static int boot_disc_copy(void)
{
	char s[160], d[160];
	int i;

	for (i = 0; i < N_BOOT_DIRS; i++)
		sysFsMkdir(boot_dirs[i], 0777);
	for (i = 0; i < N_BOOT_FILES; i++)
	{
		snprintf(s, sizeof s, "%s/%s", BOOT_SRC, boot_files[i]);
		snprintf(d, sizeof d, "%s/%s", BOOT_DST, boot_files[i]);
		if (same_file(s, d) != 1 && copy_verify(s, d) != 0)
			return -20 - i;
	}
	return 0;
}

static void boot_disc_remove(void)
{
	char d[160];
	int i;

	for (i = 0; i < N_BOOT_FILES; i++)
	{
		snprintf(d, sizeof d, "%s/%s", BOOT_DST, boot_files[i]);
		sysFsUnlink(d);
	}
	for (i = N_BOOT_DIRS - 1; i >= 0; i--)
		sysFsRmdir(boot_dirs[i]);
}

/* text with Prism's block taken out, into out: 1 = there was one, 0 = none, -1 = a begin line with
 * no end line (then the file is left alone) */
static int block_cut(const char *text, char *out, int out_n)
{
	const char *b = strstr(text, AUTO_BEGIN), *e;

	if (!b)
	{
		snprintf(out, out_n, "%s", text);
		return 0;
	}
	e = strstr(b, AUTO_END);
	if (!e)
		return -1;
	e += strlen(AUTO_END);
	if (*e == '\r')
		e++;
	if (*e == '\n')
		e++;
	snprintf(out, out_n, "%.*s%s", (int)(b - text), text, e);
	return 1;
}

/* put Prism's block at the end of one file (block != NULL; an older Prism block is replaced), or
 * take it out (block == NULL; a file that then holds only Prism's own note goes); 0 = done */
static int block_set(const char *path, const char *tmp, const char *backup, const char *nofile, const char *block)
{
	static char a[BAT_MAX + 1], cut[BAT_MAX + 1], out[BAT_MAX + 1];
	int len = read_all(path, a, BAT_MAX), had = len >= 0, c, cl;

	if (len < 0)
		len = 0;
	a[len] = 0;
	c = block_cut(a, cut, sizeof cut);
	if (c < 0)
		return -10;
	cl = (int)strlen(cut);
	if (block)
	{
		if (had && !exists(backup))
			write_all(backup, a, len);
		if (cl + (int)strlen(block) + (int)strlen(nofile) + 2 > BAT_MAX)
			return -11;
		snprintf(out, sizeof out, "%s%s%s", (had && cl) ? cut : nofile, (had && cl && cut[cl - 1] != '\n') ? "\n" : "", block);
		if (had && !strcmp(out, a))
			return 0;	/* already so */
		return swap_in(path, tmp, out);
	}
	if (!c)
		return 0;
	if (!cl || !strcmp(cut, nofile))
		return sysFsUnlink(path) == 0 ? 0 : -5;
	return swap_in(path, tmp, cut);
}

static int has_text(const char *path, const char *t)
{
	static char a[BAT_MAX + 1];
	int n = read_all(path, a, BAT_MAX);

	if (n <= 0)
		return 0;
	a[n] = 0;
	return strstr(a, t) != NULL;
}

int autostart_on(void)
{
	return has_text(AUTOEXEC, AUTO_BEGIN);
}

int autostart_set(int on, char *msg, int n)
{
	int ra = 0, rx = 0;

	if (on)
	{
		if (!exists(AUTO_BAT))
		{
			snprintf(msg, n, "This Prism is missing autostart.bat: reinstall the .pkg.");
			return -1;
		}
		rx = boot_disc_copy();
		auto_log("switch on: boot disc copy", rx);
		if (rx)
		{
			snprintf(msg, n, "Could not copy the boot disc to /dev_hdd0/tmp (%d).", rx);
			return rx;
		}
		rx = block_set(ONXMB, ONXMB_NEW, ONXMB_BACKUP2, AUTO_NOFILE_X, auto_xmb_block);
		if (!rx)
			ra = block_set(AUTOEXEC, AUTOEXEC_NEW, AUTOEXEC_BACKUP, AUTO_NOFILE_A, auto_exec_block);
		auto_log("switch on: onxmb.bat", rx);
		auto_log("switch on: autoexec.bat", ra);
		if (rx || ra)
			snprintf(msg, n, "Could not write %s (%d).", rx ? "onxmb.bat" : "autoexec.bat", rx ? rx : ra);
		else
			snprintf(msg, n, "On: Prism opens after you sign in. Hold L1 as the XMB appears to skip it.");
		return rx ? rx : ra;
	}
	ra = block_set(AUTOEXEC, AUTOEXEC_NEW, AUTOEXEC_BACKUP, AUTO_NOFILE_A, NULL);
	rx = block_set(ONXMB, ONXMB_NEW, ONXMB_BACKUP2, AUTO_NOFILE_X, NULL);
	sysFsUnlink(AUTO_MARK);
	boot_disc_remove();
	auto_log("switch off: autoexec.bat", ra);
	auto_log("switch off: onxmb.bat", rx);
	if (ra || rx)
		snprintf(msg, n, "Could not write %s (%d).", ra ? "autoexec.bat" : "onxmb.bat", ra ? ra : rx);
	else
		snprintf(msg, n, "Off: Prism no longer opens when the console starts.");
	return ra ? ra : rx;
}

/* v0.29: once at Prism's start. The marker is spent however Prism was opened; a switch left on by an
 * older Prism (v0.28's autoexec.bat block, which waited in autoexec.bat) is rewritten; and when
 * Prism came from its boot disc, webMAN unmounts that disc. */
void autostart_started(void)
{
	sysFSStat st, si;

	sysFsUnlink(AUTO_MARK);
	/* v0.36: the feature is gone; a console that had it on loses its lines, the boot disc copy and
	 * the markers here, once */
	if (autostart_on() || exists(BOOT_DST "/PRISM_BOOT.TXT"))
	{
		char m[160];

		auto_log("v0.36: start-up feature removed; lines taken out", autostart_set(0, m, sizeof m));
		sysFsRmdir("/dev_hdd0/tmp/prism_signed_in");
		sysFsUnlink("/dev_hdd0/tmp/prism_boot_ran");
	}
	(void)si;
	(void)t_main_s;
	(void)t_intro_s;
	if (sysFsStat(BOOT_DISC_MARK, &st) == 0)
	{
		char resp[256];

		/* "/mount_ps3/...", as launch.c mounts: with a game (Prism) running, webMAN ignores
		 * "/mount.ps3/unmount" and forces "/mount_ps3/unmount" (www_client.h; 11-23: HTTP 200 and
		 * the disc stayed) */
		auto_log("started from the boot disc; unmount (HTTP status)", wm_get("/mount_ps3/unmount", resp, sizeof resp));
	}
}

/* ---- the clock: has the console restarted since time t? ---- */

static u64 uptime_s(void)
{
	u64 t;

	__asm__ volatile("mftb %0" : "=r"(t));	/* the time base counts from power-on, 79.8 MHz */
	return t / 79800000ULL;
}

static u64 now_s(void)
{
	u64 s = 0, ns = 0;

	sysGetCurrentTime(&s, &ns);
	return s;
}

/* ---- onxmb.bat ---- */

static char bat[BAT_MAX + 1];

static int bat_read(void)
{
	int n = read_all(ONXMB, bat, BAT_MAX);

	if (n < 0)
		n = 0;
	bat[n] = 0;
	return n;
}

static int bat_write(const char *text)
{
	static char back[BAT_MAX + 1];
	int n = (int)strlen(text), m;

	if (write_all(ONXMB_NEW, text, n))
		return -1;
	m = read_all(ONXMB_NEW, back, BAT_MAX);
	if (m != n || memcmp(back, text, n))
	{
		sysFsUnlink(ONXMB_NEW);
		return -2;
	}
	sysFsUnlink(ONXMB);
	if (sysLv2FsRename(ONXMB_NEW, ONXMB) != 0)
		return -3;
	m = read_all(ONXMB, back, BAT_MAX);
	return (m == n && !memcmp(back, text, n)) ? 0 : -4;
}

static void classify(void)
{
	char *p = bat;

	const char *ml = MAP_LINE, *ds = DST_STAGE2;
	size_t mll = strlen(ml), dsl = strlen(ds);

	su.state = SU_NONE;
	su.line[0] = 0;
	if (strstr(bat, MARK_BEGIN) && strstr(bat, MARK_END) && strstr(bat, map_full()))
		su.state = SU_APP;
	/* a map line outside our block: the bench's script (same file), or someone else's */
	while ((p = strstr(p, ml)) != NULL)
	{
		char *b = strstr(bat, MARK_BEGIN), *e = strstr(bat, MARK_END), *eol;
		int len;

		if (b && e && p > b && p < e)
		{
			p += mll;
			continue;
		}
		eol = p;
		while (*eol && *eol != '\n' && *eol != '\r')
			eol++;
		len = (int)(eol - p);
		snprintf(su.line, sizeof su.line, "%.*s", len, p);
		if (!strncmp(p + mll, ds, dsl) && su.state != SU_APP)
			su.state = SU_BENCH;
		else if (strncmp(p + mll, ds, dsl))
		{
			su.state = SU_CONFLICT;
			return;
		}
		p = eol;
	}
}

void setup_read(void)
{
	char t[64];
	int n;

	memset(&su, 0, sizeof su);
	su.shipped = exists(SRC_STAGE2) && exists(SRC_CARRIER);
	bat_read();
	classify();
	su.stage2 = exists(DST_STAGE2) ? (su.shipped ? same_file(SRC_STAGE2, DST_STAGE2) : 1) : -1;
	su.carrier = exists(DST_CARRIER) ? (su.shipped ? same_file(SRC_CARRIER, DST_CARRIER) : 1) : -1;
	su.pending = su.state == SU_APP && exists(PENDING);

	/* after a removal: once the console has restarted, nothing points at the helper any more */
	n = read_all(REMOVED, t, sizeof t - 1);
	if (n > 0 && su.state == SU_NONE)
	{
		u64 when;

		t[n] = 0;
		when = strtoull(t, NULL, 10);
		if (when && now_s() > when + uptime_s() + 5)
		{
			sysFsUnlink(DST_STAGE2);
			sysFsUnlink(REMOVED);
			su.stage2 = -1;
		}
	}
}

int ps2_ready(char *why, int n)
{
	if (!chk.ps2_ok)
		snprintf(why, n, "PS2 games from the card do not work on this console: %s", chk.why);
	else if (su.state == SU_CONFLICT && strstr(su.line, A01_KIT_HELPER))
		snprintf(why, n, "the Prism A01 Test kit is still set up: open it, use its Remove step, restart the console, then Prism's SETTINGS");
	else if (su.state == SU_CONFLICT)
		snprintf(why, n, "another tool loads its own PS2 stage2 (onxmb.bat: %s)", su.line);
	else if (su.state == SU_NONE)
		snprintf(why, n, "PS2 play needs a one-time setup: SETTINGS on the home screen");
	else if (su.stage2 < 0 || su.carrier < 0)
		snprintf(why, n, "a setup file is missing: SETTINGS, set up again");
	else if (su.pending)
		snprintf(why, n, "setup finishes when the XMB comes up: quit Prism, then open it again");
	else
		return 1;
	return 0;
}

int setup_install(char *msg, int n)
{
	int r;

	setup_read();
	if (!chk.ps2_ok)
	{
		snprintf(msg, n, "Not set up: %s.", chk.why);
		return -1;
	}
	if (!su.shipped)
	{
		snprintf(msg, n, "Not set up: this copy of Prism is missing its setup files (reinstall the .pkg).");
		return -2;
	}
	if (su.state == SU_CONFLICT)
	{
		if (strstr(su.line, A01_KIT_HELPER))
			snprintf(msg, n, "Not set up: the Prism A01 Test kit is still set up. Open it, use its Remove step, restart the console, then set up here.");
		else
			snprintf(msg, n, "Not set up: onxmb.bat already loads another PS2 stage2 (%s).", su.line);
		return -3;
	}
	/* v0.54: the A01 helper logs to its own file, and only if the file is there (lr.h) */
	if (chk.a01 && !exists(A01_LOG))
	{
		static u8 z[0x10000];

		write_all(A01_LOG, z, sizeof z);
	}
	if (su.stage2 != 1)
	{
		if (su.stage2 == 0)
		{
			char old[80];

			snprintf(old, sizeof old, "%s.old", DST_STAGE2);
			sysFsUnlink(old);
			sysLv2FsRename(DST_STAGE2, old);
		}
		r = copy_verify(SRC_STAGE2, DST_STAGE2);
		if (r)
		{
			snprintf(msg, n, "Not set up: could not copy the PS2 helper to /dev_hdd0/tmp (%d).", r);
			return -4;
		}
	}
	sysFsMkdir(PS2ISO_DIR, 0777);
	if (su.carrier != 1)
	{
		r = copy_verify(SRC_CARRIER, DST_CARRIER);
		if (r)
		{
			snprintf(msg, n, "Not set up: could not copy the launcher image to /dev_hdd0/PS2ISO (%d).", r);
			return -5;
		}
	}
	if (exists(SRC_COVER) && same_file(SRC_COVER, DST_COVER) != 1)
		copy_verify(SRC_COVER, DST_COVER);
	sysFsUnlink(REMOVED);

	if (su.state == SU_BENCH)
	{
		snprintf(msg, n, "Files checked. onxmb.bat already loads the helper (lines Prism did not write; left as they are).");
		setup_read();
		return 0;
	}
	if (su.state == SU_NONE)
	{
		static char text[BAT_MAX + BLOCK_MAX + 4];
		const char *block = block_text();
		int len = bat_read();

		if (len + (int)strlen(block) + 3 > BAT_MAX)
		{
			snprintf(msg, n, "Not set up: onxmb.bat is too big to add to.");
			return -6;
		}
		if (!exists(BACKUP))
			r = len ? write_all(BACKUP, bat, len) : write_all(BACKUP, NO_FILE_NOTE, (int)strlen(NO_FILE_NOTE));
		snprintf(text, sizeof text, "%s%s%s", bat, (len && bat[len - 1] != '\n') ? "\n" : "", block);
		write_all(PENDING, "1\n", 2);
		r = bat_write(text);
		if (r)
		{
			sysFsUnlink(PENDING);
			snprintf(msg, n, "Not set up: could not write /dev_hdd0/onxmb.bat (%d). The old one is in %s.", r, BACKUP);
			return -7;
		}
	}
	setup_read();
	snprintf(msg, n, su.pending ? "Set up. It takes effect when the XMB comes up: quit Prism, then open it again." : "Set up.");
	return 0;
}

int setup_remove(char *msg, int n)
{
	char *b, *e, t[32];
	static char text[BAT_MAX + 1];
	int r, only_space = 1;

	setup_read();
	if (su.state == SU_BENCH || su.state == SU_CONFLICT)
	{
		snprintf(msg, n, "Nothing removed: onxmb.bat loads the helper with lines Prism did not write (%s). Take those out by hand.", su.line);
		return -1;
	}
	if (su.state == SU_APP)
	{
		b = strstr(bat, MARK_BEGIN);
		e = strstr(bat, MARK_END);
		if (!b || !e || e < b)
		{
			snprintf(msg, n, "Nothing removed: Prism's lines in onxmb.bat are not whole.");
			return -2;
		}
		e += sizeof MARK_END - 1;
		if (*e == '\r')
			e++;
		if (*e == '\n')
			e++;
		snprintf(text, sizeof text, "%.*s%s", (int)(b - bat), bat, e);
		for (b = text; *b; b++)
			if (*b > ' ')
				only_space = 0;
		r = read_all(BACKUP, t, sizeof t - 1);
		if (only_space && r > 0 && !strncmp(t, "# (there was no", 15))
			r = sysFsUnlink(ONXMB) == 0 ? 0 : -1;
		else
			r = bat_write(text);
		if (r)
		{
			snprintf(msg, n, "Nothing removed: could not write /dev_hdd0/onxmb.bat (%d).", r);
			return -3;
		}
	}
	sysFsUnlink(PENDING);
	wm_shortcuts_remove();	/* v0.55: without the helper they would only start the fallback */
	if (same_file(SRC_CARRIER, DST_CARRIER) == 1)
		sysFsUnlink(DST_CARRIER);
	if (exists(SRC_COVER) && same_file(SRC_COVER, DST_COVER) == 1)
		sysFsUnlink(DST_COVER);
	snprintf(t, sizeof t, "%llu\n", (unsigned long long)now_s());
	write_all(REMOVED, t, (int)strlen(t));
	setup_read();
	snprintf(msg, n, "Removed. PS2 games use the normal stage2 again after the console restarts.");
	return 0;
}

/* ---- v0.55: the card's PS2 games in webMAN's own list (Bananafuzzbuddy, 2026-11-24) ----
 * One shortcut per PS2 game on the card: a copy of a small template image under the game's own file
 * name in /dev_hdd0/PS2ISO/SDCARD, where webMAN lists it like any PS2 title (and never offers to copy
 * anything: the real game stays on the card). Its marker (PRISMLR1 / mode=iso / path=*) makes the
 * helper play the card file of that name, from /PS2ISO or /PS2PRISM on any partition, whole or split
 * (lr_card.c path_from_title, lr_fs.c lr_fs_open). The same mechanism as the launcher's pins (run S1,
 * Amendment F); the templates are bench/lr_title.py build_template's (app_a01/tools/mk_templates.py).
 * The names made are kept in WMSC_LIST; only those, and only while they carry the marker, are ever
 * deleted (setup_remove, and a game no longer on the card). */
#include "games.h"
#include "cardcheck.h"	/* v0.66: card_lun_for */
#define WMSC_DIR	PS2ISO_DIR "/SDCARD"		/* lr.h LR_PIN_DIR */
#define WMSC_LIST	"/dev_hdd0/game/PRSM00001/USRDIR/wm_shortcuts.txt"
#define WMSC_TPL_DVD	APP_SETUP "/tpl_dvd.iso"
#define WMSC_TPL_CD	APP_SETUP "/tpl_cd.iso"
#define WMSC_MARK_OFF	0x7800

static int wmsc_is_ours(const char *path)
{
	char m[8];
	s32 fd;
	u64 pos = 0, got = 0;

	if (sysFsOpen(path, SYS_O_RDONLY, &fd, NULL, 0) != 0)
		return 0;
	if (sysFsLseek(fd, WMSC_MARK_OFF, 0, &pos) != 0 || sysFsRead(fd, m, 8, &got) != 0)
		got = 0;
	sysFsClose(fd);
	return got == 8 && !memcmp(m, "PRISMLR1", 8);
}

static const char *leaf_of(const char *p)
{
	const char *s = strrchr(p, '/');

	return s ? s + 1 : p;
}

/* v0.66: the shortcut of a game on a card behind another card (cardcheck.h card_lun_for) carries "lun=N" in its marker,
 * in the newline padding right after "path=*" (the helper reads the marker with the same parser as its trigger file,
 * lr_card.c cfg_read). lun < 0 puts the padding back. Only a marker in exactly the template's shape is touched. */
#define WMSC_HEAD	"PRISMLR1\nmode=iso\npath=*\n"
static int wmsc_set_lun(const char *path, int lun)
{
	char m[32], want[6];
	const int at = (int)sizeof(WMSC_HEAD) - 1;
	s32 fd;
	u64 pos = 0, got = 0;
	int i, r = -1;

	memcpy(want, "\n\n\n\n\n\n", 6);
	if (lun >= 0 && lun <= 7)
		memcpy(want, "lun=", 4), want[4] = (char)('0' + lun);
	if (sysFsOpen(path, SYS_O_RDWR, &fd, NULL, 0) != 0)
		return -1;
	if (sysFsLseek(fd, WMSC_MARK_OFF, 0, &pos) == 0 && sysFsRead(fd, m, sizeof m, &got) == 0 && got == sizeof m
		&& !memcmp(m, WMSC_HEAD, at))
	{
		int pad = 1, old = !memcmp(m + at, "lun=", 4) && m[at + 4] >= '0' && m[at + 4] <= '7' && m[at + 5] == '\n';

		for (i = 0; i < 6; i++)
			pad &= m[at + i] == '\n';
		if (!memcmp(m + at, want, 6))
			r = 0;
		else if ((pad || old) && sysFsLseek(fd, WMSC_MARK_OFF + at, 0, &pos) == 0 && sysFsWrite(fd, want, 6, &got) == 0 && got == 6)
			r = 0;
	}
	sysFsClose(fd);
	return r;
}

int wm_shortcuts_add(char *msg, int n)
{
	static char list[16384];
	char dst[260], resp[200];
	int i, made = 0, had = 0, skipped = 0, len;
	FILE *f;

	setup_read();
	if (su.state != SU_APP && su.state != SU_BENCH)
	{
		snprintf(msg, n, "Set up PS2 play first (the row above).");
		return -1;
	}
	if (!exists(WMSC_TPL_DVD) || !exists(WMSC_TPL_CD))
	{
		snprintf(msg, n, "This Prism is missing its shortcut templates: reinstall the .pkg.");
		return -2;
	}
	sysFsMkdir(PS2ISO_DIR, 0777);
	sysFsMkdir(WMSC_DIR, 0777);
	len = read_all(WMSC_LIST, list, sizeof list - 1);
	list[len < 0 ? 0 : len] = 0;
	f = fopen(WMSC_LIST, "a");
	for (i = 0; i < n_games[P_PS2]; i++)
	{
		const game_t *g = &games[P_PS2][i];
		const char *nm = leaf_of(g->path);

		if (g->where != 1 || !*nm || strlen(nm) > 200)	/* the card, read raw: Prism's own card reader */
			continue;
		snprintf(dst, sizeof dst, WMSC_DIR "/%s", nm);
		if (exists(dst))
		{
			if (wmsc_is_ours(dst))
			{
				had++;
				wmsc_set_lun(dst, card_lun_for(g->slot));	/* v0.66: the cards may sit in other slots now */
			}
			else
				skipped++;	/* someone else's file of that name: left alone */
			continue;
		}
		if (copy_verify(g->kind ? WMSC_TPL_CD : WMSC_TPL_DVD, dst) != 0 || wmsc_set_lun(dst, card_lun_for(g->slot)) != 0)
		{
			sysFsUnlink(dst);
			skipped++;
			continue;
		}
		made++;
		if (f)
			fprintf(f, "%s\n", nm);
	}
	if (f)
		fclose(f);
	if (!made && !had)
	{
		snprintf(msg, n, n_games[P_PS2] ? "No shortcuts made: no PS2 game on the SD card could get one." : "No PS2 games on the SD card.");
		return -3;
	}
	wm_get("/refresh.ps3", resp, sizeof resp);	/* webMAN re-reads its game lists */
	snprintf(msg, n, "%d new in webMAN's PS2 list, folder SDCARD (%d already there%s). Start them like any PS2 game, card in.",
		made, had, skipped ? ", some skipped" : "");
	return 0;
}

/* v0.56: one game at a time, from its Square menu (Bananafuzzbuddy, 11-24: looked for it there) */
int wm_shortcut_has(const void *gv)
{
	const game_t *g = (const game_t *)gv;
	char p[260];
	const char *nm = leaf_of(g->path);

	snprintf(p, sizeof p, WMSC_DIR "/%s", nm);
	return exists(p) && wmsc_is_ours(p);
}

int wm_shortcut_one(const void *gv, int add, char *msg, int n)
{
	const game_t *g = (const game_t *)gv;
	char dst[260], resp[200];
	const char *nm = leaf_of(g->path);
	FILE *f;

	snprintf(dst, sizeof dst, WMSC_DIR "/%s", nm);
	if (!add)
	{
		if (wmsc_is_ours(dst))
			sysFsUnlink(dst);
		wm_get("/refresh.ps3", resp, sizeof resp);
		snprintf(msg, n, "%s is out of webMAN's list", g->shown);
		return 0;
	}
	setup_read();
	if (su.state != SU_APP && su.state != SU_BENCH)
	{
		snprintf(msg, n, "Set up PS2 play first: SETTINGS, \"Set up PS2 play\"");
		return -1;
	}
	if (!exists(WMSC_TPL_DVD) || !exists(WMSC_TPL_CD))
	{
		snprintf(msg, n, "This Prism is missing its shortcut templates: reinstall the .pkg");
		return -2;
	}
	if (exists(dst))
	{
		snprintf(msg, n, wmsc_is_ours(dst) ? "%s is already in webMAN's list" : "webMAN's SDCARD folder already has another file called that", g->shown);
		return 0;
	}
	sysFsMkdir(PS2ISO_DIR, 0777);
	sysFsMkdir(WMSC_DIR, 0777);
	if (copy_verify(g->kind ? WMSC_TPL_CD : WMSC_TPL_DVD, dst) != 0 || wmsc_set_lun(dst, g->where == 1 ? card_lun_for(g->slot) : -1) != 0)
	{
		sysFsUnlink(dst);
		snprintf(msg, n, "Could not write the shortcut to the hard disk");
		return -3;
	}
	f = fopen(WMSC_LIST, "a");
	if (f)
	{
		fprintf(f, "%s\n", nm);
		fclose(f);
	}
	wm_get("/refresh.ps3", resp, sizeof resp);
	snprintf(msg, n, "%s is in webMAN's PS2 list now, folder SDCARD. Start it there with the card in.", g->shown);
	return 0;
}

static void wm_shortcuts_remove(void)
{
	static char list[16384];
	char path[260], resp[200], *p;
	int len = read_all(WMSC_LIST, list, sizeof list - 1);

	if (len <= 0)
		return;
	list[len] = 0;
	for (p = strtok(list, "\n"); p; p = strtok(NULL, "\n"))
	{
		snprintf(path, sizeof path, WMSC_DIR "/%s", p);
		if (wmsc_is_ours(path))
			sysFsUnlink(path);
	}
	sysFsUnlink(WMSC_LIST);
	sysFsRmdir(WMSC_DIR);	/* only if empty: the launcher's own pins may live there too */
	wm_get("/refresh.ps3", resp, sizeof resp);
}

/* v0.16: every Prism comes with Tiny Tees (Bananafuzzbuddy's PS2 homebrew, the test disc this project was
 * built with): copied once to /dev_hdd0/PS2ISO, where Prism's PS2 list and webMAN's both find it.
 * Skipped when an identical image is there already under any name. After that, never again:
 * deleting it from Prism keeps it deleted (USRDIR/tinytees.done). */
void tinytees_install(void)
{
	sysFSDirent e;
	u64 rd;
	s32 fd;
	int have = 0;

	if (exists(TT_DONE) || !exists(SRC_TT))
		return;
	sysFsMkdir(PS2ISO_DIR, 0777);
	if (sysFsOpendir(PS2ISO_DIR, &fd) == 0)
	{
		while (!have && sysFsReaddir(fd, &e, &rd) == 0 && rd > 0)
		{
			char p[300];

			if (e.d_name[0] == '.')
				continue;
			snprintf(p, sizeof p, "%s/%s", PS2ISO_DIR, e.d_name);
			have = same_file(SRC_TT, p) == 1;
		}
		sysFsClosedir(fd);
	}
	if (have || copy_verify(SRC_TT, DST_TT) == 0)
		write_all(TT_DONE, have ? "already there\n" : "copied\n", have ? 14 : 7);
}

/* v0.32: the start-up timing line (autostart_started) */
uint64_t t_main_s, t_intro_s;

/* v0.33: in the file system's clock, so it can be set against files' times (v0.32 used the system
 * time call, and its line came out 423752314 s off: a different count) */
uint64_t wall_s(void)
{
	sysFSStat st;
	s32 fd;

	if (sysFsOpen("/dev_hdd0/tmp/prism_clock", SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, NULL, 0) == 0)
	{
		u64 w = 0;

		sysFsWrite(fd, "t", 1, &w);
		sysFsClose(fd);
	}
	if (sysFsStat("/dev_hdd0/tmp/prism_clock", &st) == 0)
		return (uint64_t)st.st_mtime;
	return now_s();
}

/* v0.59: Tiny Tees' cover (Bananafuzzbuddy 11-24: so that people are less likely to delete it), original art in the
 * game's own look (tools/mk_tinytees_cover.py). While Tiny Tees is on the hard disk: a "Tiny Tees.png" next
 * to it, which webMAN shows on the XMB (never over a picture already there), and its cover in Prism's art
 * file (covers_builtin: never over a cover already there, never back after the player removed it).
 * TT_SERIAL is the serial the shipped disc's SYSTEM.CNF names: build_app.sh refuses a package where it is not. */
#include "covers.h"
#define TT_SERIAL	"SLUS_999.01"
#define SRC_TT_PNG	APP_SETUP "/tinytees_cover.png"
#define SRC_TT_RGBA	APP_SETUP "/tinytees_cover.rgba"
#define DST_TT_PNG	"/dev_hdd0/PS2ISO/Tiny Tees.png"

int tinytees_cover(void)
{
	if (!exists(DST_TT))
		return 0;
	if (!exists(DST_TT_PNG) && exists(SRC_TT_PNG) && copy_verify(SRC_TT_PNG, DST_TT_PNG) != 0)
		sysFsUnlink(DST_TT_PNG);
	return covers_builtin(TT_SERIAL, 2, "Tiny Tees", SRC_TT_RGBA);
}
