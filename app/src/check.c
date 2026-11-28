/* The console check (v0.15, for the public release): PS2 games from the SD card need
 *   - a console whose PS2 emulator is ps2_gxemu (the graphics chip in hardware, the main CPU in
 *     software: CECHC / CECHE). Read from the console ID's product sub-code through Cobra's PS3MAPI
 *     (the same ID Cobra and webMAN read; only that one byte is kept). Cobra itself picks the
 *     emulator from the lv1 hardware config (stage2/storage_ext.c get_ps2emu_type), which a
 *     GameOS app cannot read.
 *   - exactly the ps2_gxemu.self our helper (the lr_g5 stage2) was built and tested against: it
 *     works inside that program at fixed addresses. SHA-256 of the file in flash (read only).
 *   - Cobra 8.5: its PS2 hand-over hook copies the helper in (storage_ext.c copy_ps2emu_stage2).
 *   - webMAN MOD: it mounts the games and runs onxmb.bat, which loads the helper (setup.c).
 * Nothing here writes anything. The result goes to /dev_hdd0/tmp/prism_check.log. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ppu-lv2.h>
#include <lv2/sysfs.h>
#include "check.h"
#include "sha256.h"
#include "webman.h"

#define EMU_FILE	(chk.a01 ? "/dev_flash/ps2emu/ps2_emu.self" : "/dev_flash/ps2emu/ps2_gxemu.self")
#define EMU_SIZE	(chk.a01 ? 1770208ULL : 1981120ULL)
/* CECHE01 4.93 CEX 2026-11-22; v0.54: ps2_emu 4.93 read 2026-11-24 (the A01 helper also checks 21
 * places in the emulator itself on every start, lr_probe.c, so other firmwares are let through) */
#define EMU_SHA		(chk.a01 ? "1d299edf52685772b8c6d7d6f878bd94d86c6a71824041f1224ee82faf815b8d" : \
			 "c0ec64b97590d3075e7a9a58265919c21ded72e7b7cf861278096defa76e8af9")
#define FLASH_STAGE2	(chk.a01 ? "/dev_flash/ps2emu/ps2hwemu_stage2.bin" : "/dev_flash/ps2emu/ps2gxemu_stage2.bin")
#define VERSION_TXT	"/dev_flash/vsh/etc/version.txt"
#define CHECK_LOG	"/dev_hdd0/tmp/prism_check.log"

check_t chk;

static s64 sc8(u64 a, u64 b, u64 c)
{
	lv2syscall3(8, a, b, c);
	return_to_user_prog(s64);
}

static const struct
{
	int code, family;
	const char *text;
} models[] = {
	{ 0x01, 1, "CECHA / CECHB, COK-001" },
	{ 0x02, 2, "CECHC / CECHE, COK-002" },
	{ 0x03, 2, "CECHC / CECHE, SEM-001" },	/* 2026-11-27: a CECHC reads 3 too (Reddit tester; Bananafuzzbuddy's CECHE01 reads 3) */
	{ 0x04, 3, "CECHG, DIA-001" },
	{ 0x05, 3, "CECHH, DIA-002" },
	{ 0x06, 3, "CECHJ / CECHK, VER-001" },
	{ 0x07, 3, "CECHL / M / P / Q, DYN-001" },
};

const char *model_text(int code)
{
	unsigned i;

	if (code < 0)
		return "not read";
	for (i = 0; i < sizeof models / sizeof models[0]; i++)
		if (models[i].code == code)
			return models[i].text;
	return code > 0x07 ? "a slim or super slim model" : "unknown";
}

int model_family(int code)
{
	unsigned i;

	for (i = 0; i < sizeof models / sizeof models[0]; i++)
		if (models[i].code == code)
			return models[i].family;
	return code > 0x07 ? 3 : 0;
}

static int emu_check(char *hex)
{
	sysFSStat st;
	sha256_t h;
	unsigned char d[32];
	static unsigned char b[64 * 1024];
	s32 fd;
	u64 n;

	hex[0] = 0;
	if (sysFsStat(EMU_FILE, &st) != 0 || sysFsOpen(EMU_FILE, SYS_O_RDONLY, &fd, NULL, 0) != 0)
		return -1;
	sha256_init(&h);
	while (sysFsRead(fd, b, sizeof b, &n) == 0 && n > 0)
		sha256_add(&h, b, n);
	sysFsClose(fd);
	sha256_end(&h, d);
	sha256_hex(d, hex);
	return (st.st_size == EMU_SIZE && !strcmp(hex, EMU_SHA)) ? 1 : 0;
}

static void read_fw(void)
{
	char b[256], *p;
	FILE *f = fopen(VERSION_TXT, "rb");
	int n;

	strcpy(chk.fw, "?");
	if (!f)
		return;
	n = (int)fread(b, 1, sizeof b - 1, f);
	fclose(f);
	b[n < 0 ? 0 : n] = 0;
	p = strstr(b, "release:");
	if (p && strlen(p) >= 13)
	{
		int maj = (p[8] - '0') * 10 + (p[9] - '0'), mn = (p[11] - '0') * 10 + (p[12] - '0');

		snprintf(chk.fw, sizeof chk.fw, "%d.%02d", maj, mn);
		chk.fw_num = maj * 100 + mn;
	}
	p = strstr(b, "target:");
	if (p)
	{
		char *q = strstr(p, "CEX") ? "CEX" : strstr(p, "DEX") ? "DEX" : NULL;

		if (q)
			snprintf(chk.fw + strlen(chk.fw), sizeof chk.fw - strlen(chk.fw), " %s", q);
	}
}

/* v0.22: webMAN's Auto-Play box on its setup page (setup.h: add_checkbox("apd", ...); a ticked box
 * carries checked="checked" before its closing '>'). 1 on, 0 off, -1 could not tell. */
static int wm_autoplay_state(void)
{
	int n, r = -1;
	char *b = (char *)malloc(48 * 1024), *p, *e;

	if (!b)
		return -1;
	n = wm_get_body("/setup.ps3", b, 48 * 1024);
	if (n > 0 && (p = strstr(b, "name=\"apd\"")) && (e = strchr(p, '>')))
	{
		*e = 0;
		r = strstr(p, "checked") ? 1 : 0;
	}
	free(b);
	return r;
}

/* v0.22: does this webMAN take requests from its request file? Prism starts PS3 games that way
 * (launch.c request_play); the Lite edition is built without it (webMAN-MOD flags_lite.h). A
 * harmless request ("/cpursx.ps3") is left, webMAN is poked, and the file must be gone within 2 s.
 * A request already waiting there is someone else's: left alone, answer "not checked". */
#define WMREQ_FILE	"/dev_hdd0/tmp/wm_request"

static int wm_request_works(void)
{
	static const char req[] = "/cpursx.ps3";
	sysFSStat st;
	FILE *f;
	int i;

	if (sysFsStat(WMREQ_FILE, &st) == 0)
		return -1;
	f = fopen(WMREQ_FILE, "wb");
	if (!f)
		return -1;
	fwrite(req, 1, sizeof req - 1, f);
	fclose(f);
	if (wm_poke() != 0)
	{
		sysFsUnlink(WMREQ_FILE);
		return 0;
	}
	for (i = 0; i < 20; i++)
	{
		if (sysFsStat(WMREQ_FILE, &st) != 0)
			return 1;
		usleep(100000);
	}
	sysFsUnlink(WMREQ_FILE);
	return 0;
}

/* v0.49: webMAN's Auto-Play, switched off for the player. webMAN MOD has a command that sets one
 * byte of its settings and saves them (webMAN-MOD @ 6bd44252, include/cmd/setup.h:
 * "/setup.ps3@<pos>=<value>"; not in the Lite edition), and Auto-Play is byte 582 there
 * (include/init/wm_config.h: "u8 autoplay; // [582]"). Another webMAN version may keep it somewhere
 * else, so the byte is only touched when the settings file shows 1 at 582 while the setup page shows
 * the box ticked, and it is put back if the box is still ticked afterwards. */
#define WM_CONFIG	"/dev_hdd0/tmp/wm_config.bin"
#define WM_AUTOPLAY_AT	582

static int wm_cfg_byte(long at)
{
	FILE *f = fopen(WM_CONFIG, "rb");
	int c = -1;

	if (!f)
		return -1;
	if (fseek(f, at, SEEK_SET) == 0)
		c = fgetc(f);
	fclose(f);
	return c;
}

int wm_autoplay_off(char *msg, int n)
{
	char resp[256];
	int before = wm_autoplay_state(), byte = wm_cfg_byte(WM_AUTOPLAY_AT), after;

	if (before == 0)
	{
		chk.wm_autoplay = 0;
		snprintf(msg, n, "webMAN's Auto-Play is already off.");
		return 0;
	}
	if (before != 1 || byte != 1)
	{
		snprintf(msg, n, "Prism did not find that setting where it expects it in this webMAN: untick Auto-Play in webMAN's own setup.");
		return -1;
	}
	wm_get("/setup.ps3@582=0", resp, sizeof resp);
	usleep(500000);
	after = wm_autoplay_state();
	if (after == 0 && wm_cfg_byte(WM_AUTOPLAY_AT) == 0)
	{
		chk.wm_autoplay = 0;
		snprintf(msg, n, "webMAN's Auto-Play is off now.");
		return 0;
	}
	/* it did not take: the byte goes back to what it was, whatever it stands for in this webMAN */
	wm_get("/setup.ps3@582=1", resp, sizeof resp);
	usleep(500000);
	chk.wm_autoplay = wm_autoplay_state();
	snprintf(msg, n, "This webMAN did not take the change: untick Auto-Play in webMAN's own setup.");
	return -1;
}

void check_console(void)
{
	char hex[65], body[512];
	u16 v = 0;
	u64 idps[2] = { 0, 0 };
	sysFSStat st;
	FILE *f;

	memset(&chk, 0, sizeof chk);
	chk.model = -1;
	read_fw();
	chk.cobra = sc8(0x7001, (u64)(unsigned long)&v, 0) == 0 && v >= 0x0700 && v < 0x1000;	/* SYSCALL8_OPCODE_GET_VERSION2 */
	chk.cobra_bcd = chk.cobra ? v : 0;
	if (chk.cobra && sc8(0x7777, 0x0081, (u64)(unsigned long)idps) == 0)		/* PS3MAPI GET_IDPS */
		chk.model = (int)(idps[0] & 0xFF);	/* byte 7 of the 16 (big-endian): product sub-code */
	idps[0] = idps[1] = 0;
	chk.a01 = model_family(chk.model) == 1;	/* v0.54: before the emulator file is looked at */
	chk.emu = emu_check(hex);
	chk.flash_stage2 = sysFsStat(FLASH_STAGE2, &st) == 0;
	body[0] = 0;
	chk.wm = wm_get_body("/cpursx.ps3", body, sizeof body) > 0 && (strstr(body, "webMAN") || strstr(body, "HTTP/1."));
	chk.wm_req = chk.wm ? wm_request_works() : -1;
	chk.wm_autoplay = chk.wm ? wm_autoplay_state() : -1;

	if (!chk.cobra)
		snprintf(chk.why, sizeof chk.why, "Cobra is not running (it needs Cobra CFW with its syscalls on)");
	/* v0.49 (the owner, 11-23: "could it work on consoles not on 4.93 ... if it's possible allow it,
	 * and add a disclaimer"). What the helper depends on, read in the Cobra source
	 * (xXEvilnatXx/Cobra-PS3 @ 608ac4aa):
	 *  - the PS2 emulator's addresses, ps2emu/include/ps2emu/symbols.h: the same file, byte for
	 *    byte, in every Cobra 8.2 - 8.5 tree for firmware 4.78 to 4.93;
	 *  - the stage2 Prism's helper is built on (ps2emu_stage2/gxemu, ps2emu_stage2/common, ps2emu/):
	 *    identical in Cobra 8.3 (4.84 - 4.88), 8.4 (4.89, 4.90) and 8.5 (4.91 - 4.93).
	 * So Cobra 8.3 to 8.5 is let through, and so is an emulator file whose fingerprint is not
	 * 4.93's on firmware 4.84 to 4.93 (the file may be packed differently per firmware; nobody has
	 * looked). It has only ever RUN on 4.93 with Cobra 8.5: everything else is marked untested and
	 * the player is told. The worst case is a PS2 game that does not start; nothing is in flash. */
	else if (chk.cobra_bcd < 0x0830 || chk.cobra_bcd > 0x0850)
		snprintf(chk.why, sizeof chk.why, "Cobra %x.%x is running; PS2 from the card needs Cobra 8.3, 8.4 or 8.5", chk.cobra_bcd >> 8, (chk.cobra_bcd >> 4) & 15);
	/* v0.54: CECHA / CECHB are let through (the A01 helper, setup.c / launch.c), marked untested below */
	else if (model_family(chk.model) == 3)
		snprintf(chk.why, sizeof chk.why, "this console (%s) has no PS2 hardware", model_text(chk.model));
	/* model unknown (family 0): not a refusal. On a console whose PS2 emulator is not ps2_gxemu,
	 * Cobra never copies the gxemu stage2 our map replaces, so the helper is simply not used and
	 * the launcher image boots as a plain PS2 title: nothing breaks. Only a model positively
	 * known to be another kind is refused up front. */
	else if (chk.emu < 0)
		snprintf(chk.why, sizeof chk.why, "the PS2 emulator file is missing from flash");
	else if (chk.emu == 0 && !(chk.fw_num >= 484 && chk.fw_num <= 493))
		snprintf(chk.why, sizeof chk.why, "this firmware's PS2 emulator is not the one Prism was built for (%s)", chk.fw);
	else if (!chk.flash_stage2)
		snprintf(chk.why, sizeof chk.why, "Cobra's PS2 stage2 file is missing from flash");
	else if (!chk.wm)
		snprintf(chk.why, sizeof chk.why, "webMAN MOD did not answer (it mounts the games)");
	else
		chk.ps2_ok = 1;
	if (chk.ps2_ok && chk.a01)
	{
		chk.untested = 1;
		snprintf(chk.note, sizeof chk.note, "CECHA/CECHB support is new and has not run on a real CECHA yet. If the Prism launcher starts instead of your game, the card could not be used.");
	}
	else if (chk.ps2_ok && (chk.cobra_bcd != 0x0850 || chk.fw_num != 493 || chk.emu != 1))
	{
		chk.untested = 1;
		snprintf(chk.note, sizeof chk.note, "Not tested on firmware %s with Cobra %x.%x: a PS2 game may not start. If the screen stays black, hold the power button.",
			chk.fw, chk.cobra_bcd >> 8, (chk.cobra_bcd >> 4) & 15);
	}
	chk.done = 1;

	f = fopen(CHECK_LOG, "w");
	if (f)
	{
		fprintf(f, "Prism console check\nfirmware %s\ncobra %d bcd %04x\nmodel sub-code %d = %s (family %d)\n", chk.fw, chk.cobra,
			chk.cobra_bcd, chk.model, model_text(chk.model), model_family(chk.model));
		fprintf(f, "a01 %d\n%s %s sha256 %s\nflash stage2 %d\nwebMAN %d\nps2_ok %d %s\n", chk.a01, chk.a01 ? "ps2_emu.self" : "ps2_gxemu.self",
			chk.emu > 0 ? "MATCHES" : chk.emu == 0 ? "DIFFERS" : "MISSING",
			hex, chk.flash_stage2, chk.wm, chk.ps2_ok, chk.why);
		fprintf(f, "webMAN request file %d (1 served, 0 not: Lite?), webMAN Auto-Play %d (1 on, 0 off, -1 unknown)\n", chk.wm_req, chk.wm_autoplay);
		fclose(f);
	}
}

/* SETTINGS' storage list (screens.h) */
#include "screens.h"
#include "games.h"		/* card_dev */
#include "cardcheck.h"
#include "cardcheck_core.h"	/* CC_OK */
#include "exfat_w.h"
#include "exfat_card.h"

void dev_space_scan(void)
{
	static const struct { const char *path, *name; } d[] = {
		{ "/dev_sd", "SD card" }, { "/dev_hdd0", "Internal hard disk" }, { "/dev_usb000", "USB 0" }, { "/dev_usb001", "USB 1" },
		{ "/dev_usb002", "USB 2" }, { "/dev_usb003", "USB 3" }, { "/dev_usb004", "USB 4" }, { "/dev_usb005", "USB 5" },
		{ "/dev_usb006", "USB 6" }, { "/dev_usb007", "USB 7" }, { "/dev_ms", "Memory Stick" }, { "/dev_cf", "CompactFlash" },
	};
	unsigned i;

	n_dev_space = 0;
	for (i = 0; i < sizeof d / sizeof d[0] && n_dev_space < DEV_SPACE_N; i++)
	{
		sysFSStat st;
		u32 bs = 0;
		u64 fb = 0;

		if (sysFsStat(d[i].path, &st) != 0 || sysFsGetFreeSize(d[i].path, &bs, &fb) != 0)
			continue;
		dev_space[n_dev_space].name = d[i].name;
		dev_space[n_dev_space].free = (unsigned long long)bs * fb;
		n_dev_space++;
	}
	/* v0.73 (Bananafuzzbuddy, 11-25: "the storage doesn't list the MS"): the PS3 mounts only FAT32, so a card's exFAT
	 * part was missing here although Prism reads games from it. Asked of Prism's own exFAT code, on every
	 * slot whose card passed the damage check. Read only. */
	{
		static char nm[3][32];
		u64 dev_was = card_dev;
		int s;

		for (s = 0; s < 3 && n_dev_space < DEV_SPACE_N; s++)
		{
			exw_dev dv;
			uint64_t fr = 0, tot = 0;
			char why[120];

			if (slot_cc[s] != CC_OK)
				continue;
			card_dev = slot_storage(s);
			if (exc_open(&dv, why, sizeof why) == 0)
			{
				if (exw_space(&dv, &fr, &tot, why, sizeof why) == 0)
				{
					snprintf(nm[s], sizeof nm[s], "%s (exFAT)", slot_name(s));
					dev_space[n_dev_space].name = nm[s];
					dev_space[n_dev_space].free = fr;
					n_dev_space++;
				}
				exc_close();
			}
		}
		card_dev = dev_was;
	}
}
