/* Start a game straight from the app (v0.4), no XMB in between where that is known to work:
 *   PS1: webMAN /mount_ps3 of the image (allowed while an app runs), then the app ENDS BY
 *        STARTING /dev_flash/ps1emu/ps1_emu.self with the XMB's arguments (multiMAN /
 *        IRISMAN do the same; argument list from psdevwiki "PS1 Emulation").
 *   PS3: webMAN /mount_ps3 of the ISO, wait for /dev_bdvd, end by starting its EBOOT.BIN.
 *   PS2 (v0.5): the stage2's one-shot trigger (mode=iso path=<card path>) + /mount_ps3 of the
 *        carrier title, then the XMB's own hand-over, sys_sm_shutdown(0x8202, block, 0x4F0),
 *        made from the app (launch_ps2_now).
 * webMAN's Auto-Play stays off; /mount_ps3 is answered once the mount is done (webman.c). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <lv2/sysfs.h>
#include <sys/systime.h>
#include "games.h"
#include "webman.h"
#include "launch.h"
#include "check.h"
#include "pspsd.h"
#include "cardcheck.h"	/* v0.65: card_lun */

/* v0.54: the A01 helper (CECHA / CECHB) reads its own trigger file (lr.h LR_CFG_FILE, hwemu) */
#define TRIGGER	(chk.a01 ? "/dev_hdd0/tmp/prism_a01.cfg" : "/dev_hdd0/tmp/prism_lr.cfg")
#define CARRIER	"/dev_hdd0/PS2ISO/Prism Launcher.iso"
#define GO_FILE	"/dev_hdd0/tmp/prism_go.bat"	/* v0.3's request file, never used now */
#define PS1EMU	"/dev_flash/ps1emu/ps1_emu.self"
#define PS1CARD_NAME	"PRISM_PS1_1.VM1"
#define PS1CARD	"/dev_hdd0/savedata/vmc/" PS1CARD_NAME
#define PS3BOOT	"/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN"
/* v0.23: webMAN's PSP launchers are PSN-style PSP titles (PARAM.SFO + USRDIR/ISO.BIN.EDAT, no
 * EBOOT.BIN): the XMB hands them to the PS3's PSP emulator, which Cobra points at the mounted
 * image. v0.16-v0.22 looked for USRDIR/EBOOT.BIN and said "not installed" on a console that has
 * both (owner, 2026-11-23). Their PARAM.SFO is the test now. */
#define PSPL_UMD	"/dev_hdd0/game/PSPC66820/PARAM.SFO"	/* "PSP Remaster Launcher" (category PE) */
#define PSPL_MINIS	"/dev_hdd0/game/PSPM66820/PARAM.SFO"	/* "PSP Launcher" (category MN): ran Call of Duty from the XMB, 2026-11-23 */
#define PSPL_UMD_DIR	"/dev_hdd0/game/PSPC66820"
#define PSPL_MINIS_DIR	"/dev_hdd0/game/PSPM66820"
/* v0.23: the XMB starts a PSP title by exit-spawning the PSP emulator with the title's FOLDER
 * (RPCS3 Emu/System.cpp, category PE: argv = { "/dev_flash/pspemu/psp_emulator.self",
 * "/dev_hdd0/game/<dir>" }; psdevwiki "PSP Emulation" says the ISO.BIN.EDAT path instead). With
 * the image mounted by webMAN, Cobra serves it to the emulator in place of the launcher's own. */
#define PSPEMU	"/dev_flash/pspemu/psp_emulator.self"

/* v0.26: the XMB's own "game exec" (PlayStation Home's), sprx/gameexec stub. Types (psdevwiki
 * Game_plugin, gametype): 1 disc, 2 HDD title, 3 Home. */
#include <sysmodule/sysmodule.h>
extern int cellGameExecGame(unsigned int type, const char *dir_name, unsigned int options,
	unsigned int mem_container, unsigned int exec_data, unsigned int user_data);
#define SYSMODULE_GAME_EXEC	0x37
#define START_LOG_EARLY	"/dev_hdd0/tmp/prism_start.log"

static int game_exec(unsigned int type, const char *dir, const char *image)
{
	int lr = sysModuleLoad(SYSMODULE_GAME_EXEC), r = -1;
	FILE *f = fopen(START_LOG_EARLY, "ab");

	if (f)
	{
		fprintf(f, "gameexec: module load %08x; calling cellGameExecGame(%u, \"%s\") for %s\n", (unsigned)lr, type, dir, image);
		fclose(f);
	}
	if (lr == 0)
		r = cellGameExecGame(type, dir, 0, 0xFFFFFFFFu, 0, 0);
	f = fopen(START_LOG_EARLY, "ab");
	if (f)
	{
		fprintf(f, "gameexec: cellGameExecGame returned %08x\n", (unsigned)r);
		fclose(f);
	}
	return r;
}
#define PSP_ARG_FILE	"/dev_hdd0/game/PRSM00001/USRDIR/psp_arg.txt"	/* v0.24: trials only */

/* A trigger left armed by a start that never reached the PS2 side would take over the next PS2
 * launch from anywhere: disarm it the way the stage2 does (first byte 'x'). */
void launch_disarm(void)
{
	FILE *f = fopen(TRIGGER, "r+b");
	char b[8];

	if (!f)
		return;
	if (fread(b, 1, 8, f) == 8 && !memcmp(b, "PRISMLR1", 8))
	{
		fseek(f, 0, SEEK_SET);
		fputc('x', f);
	}
	fclose(f);
}

void launch_cancel(void)
{
	sysFsUnlink(GO_FILE);
}

static int arm(const char *path, int card_lun)
{
	char t[512], back[512];
	FILE *f;
	int n;

	char lun[24] = "";

	/* v0.65/v0.66: with a card in an earlier slot too, the helper is told the game's slot (cardcheck.h card_lun_for) */
	if (card_lun >= 0)
		snprintf(lun, sizeof lun, "lun=%d\n", card_lun);
	memset(t, '\n', sizeof(t));
	n = snprintf(t, sizeof(t), "PRISMLR1\nmode=iso\npath=%s\n%s", path, lun);
	if (n <= 0 || n >= (int)sizeof(t))
		return -10;
	t[n] = '\n';
	f = fopen(TRIGGER, "wb");
	if (!f)
		return -11;
	n = (int)fwrite(t, 1, sizeof(t), f);
	fclose(f);
	if (n != (int)sizeof(t))
		return -12;
	f = fopen(TRIGGER, "rb");
	if (!f)
		return -13;
	n = (int)fread(back, 1, sizeof(back), f);
	fclose(f);
	return (n == (int)sizeof(back) && !memcmp(t, back, sizeof(t))) ? 0 : -14;
}

/* webMAN's request file: "/play.ps3<path>", read back to be sure it is whole */
#define WMREQ	"/dev_hdd0/tmp/wm_request"
#define PRESS_FILE	"/dev_hdd0/game/PRSM00001/USRDIR/start_press.txt"	/* v0.20: timing runs only */
#define START_LOG	"/dev_hdd0/tmp/prism_start.log"

static int request_play(const char *path)
{
	if (chk.wm_req == 0)
		return 0;		/* v0.22: this webMAN does not serve the file (Lite?): the player presses X */
	char enc[640], t[700], back[700];
	FILE *f;
	int n;

	/* v0.11: the disc icon, not the path. "/play.ps3<path>" mounts and then auto_play(), which
	 * checks ONCE (after setup's "wait additionally") whether the XMB's explore_plugin is up and
	 * gives up if not - after an app quits it often is not yet (owner, v0.10: Borderlands mounted,
	 * not started). "/play.ps3?col=game&seg=seg_device" goes to exec_xmb_item(), which presses X
	 * on the disc icon. The app has already mounted the game.
	 * v0.17: v0.11-v0.16 sent that alone and the game never started (owner, 2026-11-23). webMAN
	 * holds a /play.ps3 request only while GetCurrentRunningMode() != 0, which drops to 0 the
	 * moment the app ends, before the XMB takes commands; the disc icon's ICON0.PNG is already
	 * there (the disc was mounted while the app ran), so exec_xmb_item skips its icon wait and
	 * presses at once, into an XMB that is not up. Sent from the PC to a settled XMB, the same
	 * command started Borderlands 6.9 s later (2026-11-23). So the request now waits first:
	 * /wait.ps3?xmb (webMAN-MOD include/cmd/wait.h: wait_for_xmb, explore_plugin, 30 s max; held
	 * the full 30 s with a game running, 2026-11-23), 5 s more (webMAN's own onxmb.bat runs 3 s
	 * after wait_for_xmb), then the press. ";/" chains them (www_client.h multi-command; two
	 * 2 s waits took 4.0 s). Pressing a direction, Circle or L2 on the XMB during the press still
	 * cancels it (vsh.h abort_autoplay).
	 * v0.18: v0.17 worked but showed ~8 s of XMB (owner): 5 s of ours plus exec_xmb_item's own
	 * ~4.6 s (5 focus rounds, ~23 pad reads). The press is now sent as explore_plugin commands
	 * (/xmb.ps3$<cmd>, xmb_browser.h: exec_xmb_command, on the XMB only). Bench, 2026-11-23, game
	 * quit from the PC with /xmb.ps3$exit: explore_plugin is back ~6 s after the running mode
	 * drops; a press at +0 s is IGNORED (the XMB sat there), +1 s started Borderlands 2 of 2, +2 s
	 * 1 of 1; with the cursor parked on Settings the one chain moved it to the disc and started
	 * the game. The move is sent at +0 s and again at +1 s (the app's own icon has the cursor
	 * after it quits; a press without the move would start the app again).
	 * v0.19 (owner: "faster until it doesn't work"): pressing the moment /wait.ps3?xmb returns
	 * started the game 5 of 6 times (its 1 s poll means "0 s" is 0-1 s after explore_plugin);
	 * 0.25 / 0.5 / 0.75 s each 1 of 1. A press at 0 s with a second press at +1 s: 3 of 3
	 * started and stayed in game for the next 15 s (the second press lands harmlessly). So: move
	 * + press at once, and again 1 s later as the backup.
	 * v0.20: v0.19 from Prism RESTARTED PRISM (owner, 2026-11-23): after an app quits, the XMB puts
	 * the cursor back on the app's icon AFTER explore_plugin answers, over our move, and the
	 * press at +0 hit the app. The bench trials above quit a game started from the disc icon, so
	 * the cursor was already there and they could not see it. Prism-shaped trials (Prism quit
	 * from the PC): press at +1 s (moves at 0, 1) ignored 1 of 1; press at +2 s (moves at 0, 1,
	 * 2) started Borderlands 1 of 1. Default = the +2 s chain. For timing runs,
	 * USRDIR/start_press.txt (one line, must start "/wait.ps3?xmb;") replaces it; the chain sent is
	 * written to /dev_hdd0/tmp/prism_start.log.
	 * v0.21: NONE of the v0.18-v0.20 runs from Prism counted: the chain was built with
	 * snprintf(t, n, "<chain>") and its "%20" spaces were read as conversions ("%20g", "%20s");
	 * webMAN got "focus_category 1.71274e+194ame", the cursor never moved and the press
	 * restarted Prism (prism_start.log showed it). With the strings right (timing file), Prism
	 * runs pressing at +1 / +2 s moved the cursor to the disc but the press did not start it
	 * (owner: "highlighting the cd but it isn't starting it"), and the bare exec_push presses
	 * whatever is under the cursor even with NO disc mounted (my PC test opened webMAN's own
	 * items). The only start that worked from Prism is v0.17's webMAN press, which also checks
	 * that /dev_bdvd holds a PS3 game before pressing (vsh.h exec_xmb_item). Back to it, with
	 * 2 s of ours instead of 5.
	 * v0.21 with 2 s did NOT start the game from Prism (owner, 2026-11-23); the owner asked for
	 * v0.17 back, and the v0.17 package was reinstalled as is. This is v0.17's request again
	 * (5 s). Do not shorten it without a run FROM PRISM that proves the shorter one. */
	(void)enc;
	(void)path;
	n = snprintf(t, sizeof(t), "%s", "/wait.ps3?xmb;/wait.ps3?5;/play.ps3?col=game&seg=seg_device");
	f = fopen(PRESS_FILE, "rb");
	if (f)
	{
		char o[600];
		int k = (int)fread(o, 1, sizeof(o) - 1, f);

		fclose(f);
		while (k > 0 && (unsigned char)o[k - 1] <= ' ')
			k--;
		o[k > 0 ? k : 0] = 0;
		if (k > 14 && !memcmp(o, "/wait.ps3?xmb;", 14) && !strpbrk(o, "\r\n"))
			n = snprintf(t, sizeof(t), "%s", o);
	}
	if (n <= 5 || n >= (int)sizeof(t))
		return -50;
	f = fopen(START_LOG, "ab");
	if (f)
	{
		/* v0.22: appended, with the console's uptime (time base from power-on, 79.8 MHz), for the
		 * start that did nothing right after a restart (owner, 2026-11-23) */
		u64 tb;

		__asm__ volatile ("mftb %0" : "=r"(tb));
		fprintf(f, "uptime %llu s  autoplay %d  %s\n", (unsigned long long)(tb / 79800000ULL), chk.wm_autoplay, t);
		fclose(f);
	}
	f = fopen(WMREQ, "wb");
	if (!f)
		return -51;
	if ((int)fwrite(t, 1, n, f) != n)
	{
		fclose(f);
		return -52;
	}
	fclose(f);
	f = fopen(WMREQ, "rb");
	if (!f)
		return -53;
	memset(back, 0, sizeof(back));
	fread(back, 1, sizeof(back) - 1, f);
	fclose(f);
	return memcmp(t, back, n) ? -54 : 0;
}

static int mount(const char *path, char *why, int why_n)
{
	char enc[640], url[700], resp[200];
	int r;

	wm_urlenc(enc, sizeof(enc), path);
	snprintf(url, sizeof(url), "/mount_ps3%s", enc);
	r = wm_get(url, resp, sizeof(resp));
	if (r != 200)
	{
		snprintf(why, why_n, "webMAN did not take the mount (%d)", r);
		return r < 0 ? r : -r;
	}
	return 0;
}

/* v0.27: the disc (mounted by webMAN, or real) started by the XMB itself through game exec, as PSP
 * titles are since v0.26 (cellGameExecGame(2, "PSPM66820") returned 0 and Call of Duty started,
 * owner 2026-11-23). psdevwiki Game_plugin: gameExec's DirName is passed for type 1 (disc) too, so
 * the disc's title ID is tried first, then "". 0 = the XMB will start it when Prism quits. */
/* the PS1 emulator and its arguments (v0.30: shared by image files and the disc in the drive):
 * memory card 1 (the card made for the app, 2026-11-22; empty = none, as the XMB passes an
 * unassigned slot), memory card 2, region / target ID, screen-saver seconds, upscaler normal,
 * smoothing off. The cards go as FILE NAMES: the emulator opens them in /dev_hdd0/savedata/vmc
 * itself. v0.4-v0.9 passed the full path and the game said "no memory card" (owner, MGS,
 * 2026-11-22 20:4x); IRISMAN's psx_launch passes the name ("Internal_MC.VM1"), as psdevwiki's
 * netemu table says. */
static void ps1_args(spawn_t *sp)
{
	sysFSStat st;

	snprintf(sp->self, sizeof(sp->self), "%s", PS1EMU);
	sp->argv[0] = sysFsStat(PS1CARD, &st) == 0 ? PS1CARD_NAME : "";
	sp->argv[1] = "";
	sp->argv[2] = "0082";
	sp->argv[3] = "1200";
	sp->argv[4] = "1";
	sp->argv[5] = "0";
	sp->argv[6] = NULL;
}

static int disc_exec(const game_t *g)
{
	if (g->id[0] && game_exec(1, g->id, g->path) == 0)
		return 0;
	return game_exec(1, "", g->path);
}

/* ---- v0.61: which PSP launcher a game starts with (Bananafuzzbuddy 11-24: "a setting for psp games to choose the
 * launcher it uses"; some games freeze under the minis launcher and run under the remaster one).
 * USRDIR/psp_launcher.txt, one "key=minis|remaster" a line: key "default" (SETTINGS), or a game's DISC_ID
 * (its file name when it has none; the Square menu). Nothing there: minis when installed, as before. */
#define PSPL_FILE	"/dev_hdd0/game/PRSM00001/USRDIR/psp_launcher.txt"

static int pl_read(const char *key)
{
	char ln[300];
	FILE *f = fopen(PSPL_FILE, "rb");
	int r = 0;

	if (!f)
		return 0;
	while (fgets(ln, sizeof ln, f))
	{
		char *eq = strrchr(ln, '=');

		ln[strcspn(ln, "\r\n")] = 0;
		if (!eq)
			continue;
		*eq = 0;
		if (!strcmp(ln, key))
			r = !strcmp(eq + 1, "remaster") ? PSPL_REMASTER : !strcmp(eq + 1, "minis") ? PSPL_MINIS_ : !strcmp(eq + 1, "ppsspp") ? PSPL_PPSSPP : 0;
	}
	fclose(f);
	return r;
}

int psp_launchers_installed(void)
{
	sysFSStat st;

	return (sysFsStat(PSPL_MINIS, &st) == 0 ? PSPL_MINIS_ : 0) | (sysFsStat(PSPL_UMD, &st) == 0 ? PSPL_REMASTER : 0)
		| (sysFsStat(PSPL_PPSSPP_SFO, &st) == 0 ? PSPL_PPSSPP : 0);	/* v0.69 */
}

const char *psp_game_key(const game_t *g)
{
	const char *s = strrchr(g->path, '/');

	return g->id[0] ? g->id : s ? s + 1 : g->path;
}

int psp_launcher_pref(const char *key)
{
	return pl_read(key ? key : "default");
}

/* the launcher the game will start with: its own choice, else the default, else minis; one that is not
 * installed gives way to the other */
int psp_launcher_for(const game_t *g)
{
	int have = psp_launchers_installed(), want = g ? pl_read(psp_game_key(g)) : 0;

	if (!want)
		want = pl_read("default");
	if (!want)
		want = PSPL_MINIS_;
	return (have & want) ? want : have & PSPL_MINIS_ ? PSPL_MINIS_ : have & PSPL_REMASTER ? PSPL_REMASTER : 0;
}

/* ---- v0.72: PPSSPP and a game that is not on the hard disk (Bananafuzzbuddy, 11-25: "it won't launch with my SD card
 * only HDD can we change that") ----
 * PPSSPP lists and opens /dev_hdd0/PSPISO and nothing else (its source: PS3/frontend/Main.cpp). So the game
 * gets a NAME there: an empty file called as the game is, and a redirect of that one path to the game on the
 * card, asked of webMAN (/remap.ps3<path>&to=<path>: Cobra's path map, which lives in the kernel's memory
 * until the console restarts; lr/bench used the same call for the PS2 helper, 2026-11-20). PPSSPP then sees
 * the name in its list and reads the card through it. Only places GameOS itself can read: a card slot's
 * FAT32 part, a USB drive. A game on a card's exFAT part cannot be reached this way.
 *
 * The danger is in undoing it: while the redirect lives, the name on the hard disk IS the game on the card,
 * and deleting the name would delete the game. /unmap.ps3 is not used (lr/README: it leaves the path
 * unreadable until a restart). So an empty file is deleted only when BOTH hold: the console has restarted
 * since the file was made (the redirect is gone), and the path reads as an empty file. Until then it stays,
 * and games.c passes over it (pps_is_link) so the game is not listed twice. */
#define PPS_LINKS	"/dev_hdd0/game/PRSM00001/USRDIR/ppsspp_links.txt"	/* "<clock seconds when made> TAB <path>" a line */
#define PPS_MAX		32

static struct
{
	u64 when;
	char path[300];
} pps[PPS_MAX];
static int n_pps = -1;	/* -1 = not read yet */

static u64 pps_uptime_s(void)
{
	u64 t;

	__asm__ volatile("mftb %0" : "=r"(t));	/* the time base counts from power-on, 79.8 MHz (setup.c) */
	return t / 79800000ULL;
}

static u64 pps_now_s(void)
{
	u64 s = 0, ns = 0;

	sysGetCurrentTime(&s, &ns);
	return s;
}

static void pps_read(void)
{
	FILE *f = fopen(PPS_LINKS, "rb");
	char ln[400];

	n_pps = 0;
	if (!f)
		return;
	while (n_pps < PPS_MAX && fgets(ln, sizeof ln, f))
	{
		char *tab = strchr(ln, '\t');

		ln[strcspn(ln, "\r\n")] = 0;
		if (!tab || strncmp(tab + 1, PSPL_PPSSPP_DIR, strlen(PSPL_PPSSPP_DIR)) != 0 || strlen(tab + 1) >= sizeof pps[0].path)
			continue;
		pps[n_pps].when = strtoull(ln, NULL, 10);
		strcpy(pps[n_pps].path, tab + 1);
		n_pps++;
	}
	fclose(f);
}

static void pps_write(void)
{
	FILE *f = fopen(PPS_LINKS, "wb");
	int i;

	if (!f)
		return;
	for (i = 0; i < n_pps; i++)
		fprintf(f, "%llu\t%s\n", (unsigned long long)pps[i].when, pps[i].path);
	fclose(f);
}

int pps_is_link(const char *path)
{
	int i;

	if (n_pps < 0)
		pps_read();
	for (i = 0; i < n_pps; i++)
		if (!strcmp(pps[i].path, path))
			return 1;
	return 0;
}

void pps_links_clean(void)
{
	int i, k = 0;

	if (n_pps < 0)
		pps_read();
	for (i = 0; i < n_pps; i++)
	{
		sysFSStat st;
		int restarted = pps[i].when && pps_now_s() > pps[i].when + pps_uptime_s() + 5;

		if (restarted && sysFsStat(pps[i].path, &st) != 0)
			continue;		/* gone already: forget it */
		if (restarted && st.st_size == 0 && (st.st_mode & 0170000) != 0040000 && sysFsUnlink(pps[i].path) == 0)
			continue;		/* an empty file and no redirect alive: deleted */
		pps[k++] = pps[i];
	}
	if (k != n_pps)
	{
		n_pps = k;
		pps_write();
	}
}

/* 0 = PPSSPP will find the game under its own name in PSPL_PPSSPP_DIR */
static int pps_link(const game_t *g, char *why, int why_n)
{
	static const char *const ok_at[] = { "/dev_sd/", "/dev_ms/", "/dev_cf/", "/dev_usb" };
	const char *base = strrchr(g->path, '/');
	char link[300], ea[700], eb[700], url[1500], resp[200];
	sysFSStat st;
	s32 fd;
	int i, r, reach = 0;

	base = base ? base + 1 : g->path;
	for (i = 0; i < 4; i++)
		if (!strncmp(g->path, ok_at[i], strlen(ok_at[i])))
			reach = 1;
	if (!reach || g->where == 1)
	{
		snprintf(why, why_n, "PPSSPP can play it from the hard disk, a USB drive or a card's first (FAT32) part only. Copy it to one of those (Square)");
		return -45;
	}
	if (snprintf(link, sizeof link, "%s%s", PSPL_PPSSPP_DIR, base) >= (int)sizeof link)
	{
		snprintf(why, why_n, "the game's file name is too long");
		return -45;
	}
	if (n_pps < 0)
		pps_read();
	if (!pps_is_link(link))
	{
		if (sysFsStat(link, &st) == 0)
		{
			snprintf(why, why_n, "a file of the same name is already in the hard disk's PSPISO folder: PPSSPP would play that one");
			return -47;
		}
		if (n_pps >= PPS_MAX)
		{
			snprintf(why, why_n, "too many card games were opened in PPSSPP since the console started: restart it");
			return -47;
		}
		sysFsMkdir("/dev_hdd0/PSPISO", 0777);
		if (sysFsOpen(link, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, NULL, 0) != 0)
		{
			snprintf(why, why_n, "could not make the game's name in the hard disk's PSPISO folder");
			return -47;
		}
		sysFsClose(fd);
		sysFsChmod(link, 0666);
		pps[n_pps].when = pps_now_s();
		strcpy(pps[n_pps].path, link);
		n_pps++;
		pps_write();
	}
	wm_urlenc(ea, sizeof ea, link);
	wm_urlenc(eb, sizeof eb, g->path);
	snprintf(url, sizeof url, "/remap.ps3%s&to=%s", ea, eb);
	r = wm_get(url, resp, sizeof resp);
	{
		FILE *f = fopen("/dev_hdd0/tmp/prism_start.log", "a");

		if (f)
		{
			fprintf(f, "ppsspp link: %s -> %s, webMAN %d\n", link, g->path, r);
			fclose(f);
		}
	}
	if (r != 200)
	{
		snprintf(why, why_n, "webMAN did not take the redirect to the card (%d)", r);
		return -48;
	}
	/* in force? the name must now read as the game: its size, not an empty file's */
	if (sysFsStat(link, &st) != 0 || st.st_size == 0 || (g->size && st.st_size != g->size))
	{
		snprintf(why, why_n, "the redirect to the card is not in force (the name still reads as %llu bytes)", sysFsStat(link, &st) == 0 ? (unsigned long long)st.st_size : 0ull);
		return -48;
	}
	return 0;
}

/* key NULL = the default; pref 0 = no line for the key */
int psp_launcher_set(const char *key, int pref)
{
	static char out[16384];
	char ln[300];
	FILE *f;
	int k = 0;

	if (!key)
		key = "default";
	if (strchr(key, '\n') || strlen(key) > 200)
		return -1;
	f = fopen(PSPL_FILE, "rb");
	if (f)
	{
		while (fgets(ln, sizeof ln, f) && k < (int)sizeof out - 300)
		{
			char *eq;

			ln[strcspn(ln, "\r\n")] = 0;
			eq = strrchr(ln, '=');
			if (!ln[0] || (eq && (int)(eq - ln) == (int)strlen(key) && !strncmp(ln, key, strlen(key))))
				continue;	/* the old line for this key goes */
			k += snprintf(out + k, sizeof out - k, "%s\n", ln);
		}
		fclose(f);
	}
	if (pref)
		k += snprintf(out + k, sizeof out - k, "%s=%s\n", key, pref == PSPL_REMASTER ? "remaster" : pref == PSPL_PPSSPP ? "ppsspp" : "minis");
	f = fopen(PSPL_FILE, "wb");
	if (!f)
		return -2;
	fwrite(out, 1, k, f);
	return fclose(f) == 0 ? 0 : -3;
}

int launch_prepare(int plat, const game_t *g, spawn_t *sp, char *why, int why_n)
{
	sysFSStat st;
	int r, i;

	why[0] = 0;
	memset(sp, 0, sizeof(*sp));
	if (g->kind == 4)
	{
		/* v0.30: a PS1 or PS2 disc is started the way the XMB starts one, by the same programs
		 * Prism uses for image files, with nothing mounted so they read the drive: PS2 = the
		 * hand-over (launch_ps2_now, no trigger armed: the stage2 serves the disc as it is),
		 * PS1 = ps1_emu.self. The older path below needs /dev_bdvd, which these discs never get. */
		if (plat == P_PS2 && chk.ps2_ok)
		{
			launch_disarm();
			sp->ps2 = 1;
			return 0;
		}
		if (plat == P_PS1 && sysFsStat(PS1EMU, &st) == 0)
		{
			ps1_args(sp);
			return 0;
		}
		/* v0.15: the disc in the drive (any platform): nothing to mount; webMAN presses X on the
		 * disc icon once the app has quit, as for PS3 games (request_play) */
		if (disc_exec(g) == 0)
		{
			sp->self[0] = 0;	/* v0.27: the XMB starts it once Prism has quit */
			return 0;
		}
		r = request_play(g->path);
		if (r)
		{
			snprintf(why, why_n, "could not leave webMAN the start request (%d)", r);
			return r;
		}
		if (wm_poke() != 0)
		{
			sysFsUnlink(WMREQ);
			snprintf(why, why_n, "webMAN did not answer");
			return -42;
		}
		sp->self[0] = 0;
		return 0;
	}
	if (plat == P_PSP)
	{
		/* v0.16: webMAN's /mount_ps3 of a file in a PSPISO folder makes Cobra load it as the PSP
		 * emulator's UMD (webMAN-MOD cobra.c cobra_set_psp_umd); webMAN then leaves you to start the
		 * PSP launcher on the XMB. The app starts the launcher itself. */
		/* v0.61: the launcher chosen for this game, or the default (SETTINGS) */
		const int which = psp_launcher_for(g);
		const char *pspl = which == PSPL_MINIS_ ? PSPL_MINIS : which == PSPL_REMASTER ? PSPL_UMD : NULL;

		if (which == PSPL_PPSSPP)
		{
			/* v0.69: PPSSPP (launch.h). Nothing is mounted: it reads the game's file itself, from the one
			 * folder it looks in. It is started the way the PSP launchers are (cellGameExecGame, type 2 =
			 * a title on the hard disk); it has no way to be handed the game. */
			if (strncmp(g->path, PSPL_PPSSPP_DIR, strlen(PSPL_PPSSPP_DIR)) != 0)
			{
				/* v0.72: not on the hard disk: give it a name there that leads to it (pps_link) */
				r = pps_link(g, why, why_n);
				if (r)
					return r;
			}
			r = game_exec(2, "PPSSPP001", g->path);
			sp->self[0] = 0;
			if (r)
			{
				snprintf(why, why_n, "PPSSPP could not be started (%08x)", (unsigned)r);
				return -46;
			}
			sp->ppsspp = 1;
			return 0;
		}
		if (!pspl)
		{
			snprintf(why, why_n, "the PSP launcher (PSPC66820) is not installed: webMAN's setup can install it");
			return -43;
		}
		/* v0.60: a card's PSP game is mounted (and checked) by pspsd_prepare already */
		r = pspsd_mounted(g->path) ? 0 : mount(g->path, why, why_n);
		if (r)
			return r;
		/* v0.23: started straight from Prism, as the XMB does (see PSPEMU). webMAN's own
		 * advice after the mount is to start PSP Launcher on the XMB; moving the XMB cursor to
		 * it by folder name (focus_segment_hit_first gameDir PSPM66820) did nothing (2026-11-23). */
		if (sysFsStat(PSPEMU, &st) != 0)
		{
			snprintf(why, why_n, "%s is missing", PSPEMU);
			return -44;
		}
		/* v0.24: the launcher FOLDER (v0.23, RPCS3's form) gave a black screen from both the card
		 * and the HDD (owner, 2026-11-23); IRISMAN and webMAN never start the emulator themselves
		 * (IRISMAN mount_game.h: "Use PSP Remaster Launcher to play the mounted game"). Default
		 * now psdevwiki's form, the launcher's ISO.BIN.EDAT; USRDIR/psp_arg.txt (one line, an
		 * absolute path) replaces the argument for trials. */
		/* v0.25: what the XMB really passes, read out of a running emulator started from PSP
		 * Launcher (PS3MAPI dump of psp_emulator.self's memory, 0x00C882A7, 2026-11-23):
		 *   03 "/dev_flash/pspemu/psp_emulator.self" "/dev_hdd0/game/PSPM66820" "0"
		 * i.e. the launcher's FOLDER and a second argument "0". v0.23-v0.24 passed one argument
		 * (folder, or ISO.BIN.EDAT) and got a black screen each time. The NPUMDIMG header and
		 * content ID beside it are read by the emulator from ISO.BIN.EDAT itself. psp_arg.txt
		 * (arguments separated by '|') still replaces them for trials.
		 * v0.26: v0.25's exact arguments were black too. The XMB starts a PSP title itself
		 * (basic_plugins.sprx 0x27594-0x27648): it creates the emulator AND psp_translator.self,
		 * both with a launch block holding the title ID; an app can exit-spawn only one program,
		 * so the emulator sat waiting (no translator process; PS3MAPI process list). The XMB's
		 * own start is open to apps through cellGameExec (libsysutil_game_exec.sprx, sysmodule
		 * 0x37), the PlayStation Home "game exec": cellGameExecGame(type 2 = HDD title, folder).
		 * Tried first; without psp_arg.txt the emulator is not exit-spawned any more. */
		if (sysFsStat(PSP_ARG_FILE, &st) != 0)
		{
			r = game_exec(2, which == PSPL_MINIS_ ? "PSPM66820" : "PSPC66820", g->path);
			if (r == 0)
			{
				sp->self[0] = 0;	/* the XMB starts it once Prism has quit */
				return 0;
			}
			sp->self[0] = 0;
			sp->psp = 1;		/* refused: the player presses PSP Launcher */
			return 0;
		}
		{
			static char arg[200];
			FILE *fa = fopen(PSP_ARG_FILE, "rb");
			int k = 0, na = 0;
			char *p;

			snprintf(arg, sizeof arg, "%s|0", which == PSPL_MINIS_ ? PSPL_MINIS_DIR : PSPL_UMD_DIR);
			if (fa)
			{
				char o[200];

				k = (int)fread(o, 1, sizeof o - 1, fa);
				fclose(fa);
				while (k > 0 && (unsigned char)o[k - 1] <= ' ')
					k--;
				o[k > 0 ? k : 0] = 0;
				if (k > 1 && o[0] == '/' && !strpbrk(o, "\r\n"))
					snprintf(arg, sizeof arg, "%s", o);
			}
			fa = fopen(START_LOG, "ab");
			if (fa)
			{
				fprintf(fa, "psp: %s [%s] (image %s)\n", PSPEMU, arg, g->path);
				fclose(fa);
			}
			snprintf(sp->self, sizeof(sp->self), "%s", PSPEMU);
			for (p = arg; p && na < 4; na++)
			{
				char *bar = strchr(p, '|');

				sp->argv[na] = p;
				if (bar)
					*bar++ = 0;
				p = bar;
			}
			sp->argv[na] = NULL;
		}
		return 0;
	}
	if (plat == P_PS1)
	{
		if (sysFsStat(PS1EMU, &st) != 0)
		{
			snprintf(why, why_n, "%s is missing", PS1EMU);
			return -40;
		}
		r = mount(g->path, why, why_n);
		if (r)
			return r;
		ps1_args(sp);
		return 0;
	}
	if (plat == P_PS3)
	{
		r = mount(g->path, why, why_n);
		if (r)
			return r;
		for (i = 0; i < 50 && sysFsStat(PS3BOOT, &st) != 0; i++)
			usleep(100000);
		if (i == 50)
		{
			snprintf(why, why_n, "the disc did not appear at /dev_bdvd");
			return -41;
		}
		/* v0.10: webMAN starts it from the XMB. v0.4-v0.9 exit-spawned the disc's EBOOT.BIN, and
		 * Borderlands went straight back to the XMB (owner, 2026-11-22): a disc game started that
		 * way runs in the launching app's folder and does not find its trophy file (aldostools,
		 * psx-place "Improving direct boot feature"). The app leaves webMAN a one-shot request
		 * (v0.21: wait for the XMB, 2 s, then webMAN presses X on the disc icon), makes webMAN read it, and
		 * quits.
		 * v0.27: the XMB's own game exec first (disc_exec); webMAN's press only if it refuses. */
		if (disc_exec(g) == 0)
		{
			sp->self[0] = 0;
			return 0;
		}
		r = request_play(g->path);
		if (r)
		{
			snprintf(why, why_n, "could not leave webMAN the start request (%d)", r);
			return r;
		}
		if (wm_poke() != 0)
		{
			sysFsUnlink(WMREQ);
			snprintf(why, why_n, "webMAN did not answer");
			return -42;
		}
		sp->self[0] = 0;		/* quit to the XMB */
		return 0;
	}

	if (g->where == 2)
	{
		/* v0.16: a PS2 image on the hard disk (Tiny Tees ...): mounted like any PS2 title; with no
		 * trigger armed and no marker in the image, the stage2 serves the image as it is */
		launch_disarm();
		r = mount(g->path, why, why_n);
		if (r)
			return r;
		sp->ps2 = 1;
		return 0;
	}
	/* PS2: trigger + carrier, then launch_ps2_now() hands over as the XMB does */
	if (sysFsStat(CARRIER, &st) != 0)
	{
		snprintf(why, why_n, "the carrier title %s is missing", CARRIER);
		return -20;
	}
	{
		/* v0.66: the game's own card slot; the A01 helper (CECHA / CECHB) has no lun= line and reads the first slot */
		int lun = g->where == 1 ? card_lun_for(g->slot) : -1;

		if (lun >= 0 && chk.a01)
		{
			snprintf(why, why_n, "on this console the PS2 side reads only the first card slot with a card in it: take out the other card(s) to play this one");
			return -21;
		}
		r = arm(g->path, lun);
	}
	if (r)
	{
		snprintf(why, why_n, "could not write the trigger file (%d)", r);
		return r;
	}
	r = mount(CARRIER, why, why_n);
	if (r)
	{
		launch_disarm();
		return r;
	}
	sp->ps2 = 1;
	return 0;
}

/* The XMB's own PS2 start (vsh.self 0xD4E24): sys_sm_shutdown(0x8202, block, 0x4F0) reboots
 * into the PS2 LPAR. Cobra's hook on that call (storage_ext.c shutdown_copy_params_patched)
 * records the mounted carrier in /tmp/cfg.bin and copies our stage2 in; the stage2 then serves
 * the card file named by the trigger. The block is the one this console's XMB sent (ps2_block.h)
 * with the clock refreshed. Returns only if lv2 refuses (the code says why). */
#include <ppu-lv2.h>
#include "ps2_block.h"

#define TURNOFF	"/dev_hdd0/tmp/turnoff"

static u8 ps2_blk[PS2_BLOCK_SZ] __attribute__((aligned(16)));

static s32 time_now(u64 *sec, u64 *nsec)
{
	lv2syscall2(145, (u64)sec, (u64)nsec);
	return_to_user_prog(s32);
}

static s32 sm_shutdown(u16 op, void *param, u64 size)
{
	lv2syscall4(379, op, (u64)param, size, 0);
	return_to_user_prog(s32);
}

int launch_ps2_now(void)
{
	u64 sec = 0, nsec = 0;
	int i;

	memcpy(ps2_blk, ps2_block_tpl, PS2_BLOCK_SZ);
	if (time_now(&sec, &nsec) == 0 && sec)
		for (i = 0; i < 8; i++)
			ps2_blk[i] = (u8)(sec >> (56 - 8 * i));
	/* The XMB makes /dev_hdd0/tmp/turnoff at boot and deletes it when it powers down or goes to
	 * PS2 mode; still there at the next boot = "the system was not turned off properly". Every
	 * app that powers the console down itself deletes it first (webMAN's DELETE_TURNOFF, its
	 * updater: sysLv2FsUnlink(TMP_DIR "/turnoff") before syscall 379). v0.5-v0.9 did not, and the
	 * owner got the warning after every PS2 game started from the app (2026-11-22). */
	sysFsUnlink(TURNOFF);
	return sm_shutdown(0x8202, ps2_blk, PS2_BLOCK_SZ);
}
