/* What this console is, and whether PS2 games from the SD card can work on it (check.c). */
#ifndef PRISM_APP_CHECK_H
#define PRISM_APP_CHECK_H

typedef struct
{
	int done;
	int cobra;		/* Cobra's syscall 8 answered */
	unsigned cobra_bcd;	/* 0x0850 = Cobra 8.5 */
	int model;		/* the console ID's product sub-code (board), -1 = not read */
	int emu;		/* 1 = the PS2 emulator file is the one our helper was built for, 0 = another, -1 = missing */
	int wm;			/* webMAN MOD answered on 127.0.0.1:80 */
	int wm_req;		/* v0.22: 1 = webMAN serves /dev_hdd0/tmp/wm_request (how Prism starts PS3 games), 0 = not (Lite edition), -1 = not checked */
	int wm_autoplay;	/* v0.22: webMAN's own Auto-Play: 1 on, 0 off, -1 unknown */
	int flash_stage2;	/* Cobra's /dev_flash/ps2emu/ps2gxemu_stage2.bin is there */
	char fw[24];		/* "4.93 CEX", from /dev_flash/vsh/etc/version.txt */
	int fw_num;		/* v0.49: 493 for 4.93; 0 = not read */
	int untested;		/* v0.49: PS2 from the card is allowed here but was never run on this firmware / Cobra */
	char note[170];		/* when untested: what to tell the player */
	int ps2_ok;		/* everything PS2-from-the-card needs is here */
	char why[160];		/* when ps2_ok == 0: the first thing missing, for the player */
} check_t;

extern check_t chk;

void check_console(void);
/* v0.49: turn webMAN's own Auto-Play off (the one webMAN setting Prism needs changed), through
 * webMAN's own one-setting command, only when the setting is where Prism expects it. 0 = off now. */
int  wm_autoplay_off(char *msg, int n);
const char *model_text(int code);	/* "CECHC / CECHE (COK-002)" ... */
int  model_family(int code);		/* 0 unknown, 1 PS2 chips (EE + GS), 2 GS only, 3 no PS2 hardware */
#endif
