/* v0.46: HELP (Settings -> Help) and the terms of use (Bananafuzzbuddy, 11-23: "a help section in the settings
 * for problems you think will come up and how to solve them", and terms "so that I'm not liable for
 * anything going wrong on someone else's console and they use it at their own risk").
 * One topic a page, LEFT / RIGHT turn the pages. The terms are the last page, and the screen every
 * console shows once before Prism does anything (main.c; kept in USRDIR/terms_ok.txt). The same words
 * are in TERMS.md, which goes with every download. Plain C: the PC renderer draws these pages too. */
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "look.h"
#include "screens.h"
#include "help.h"
#include "version.h"

#define TERMS_FILE	"/dev_hdd0/game/PRSM00001/USRDIR/terms_ok.txt"

/* Keep in step with TERMS.md (TERMS_VER there and in help.h). */
static const char terms_text[] =
	"Prism is a free hobby project. It comes as it is, with no warranty of any kind.\n"
	"You use it at your own risk. Its author is not responsible for any damage or loss, to a console, "
	"its disc drive, discs, storage devices, saved games or anything else, however it happens.\n"
	"Prism needs custom firmware. Putting custom firmware on a console, and whatever follows from "
	"that, is your own decision and your own risk.\n"
	"Copy only discs you own, for your own use, where your law allows it.\n"
	"Prism is not made, approved or supported by Sony.\n"
	"If you do not agree, do not install or use Prism.";

typedef struct
{
	const char *title, *body;
} topic_t;

static const topic_t topics[] = {
	{ "A PS2 game on the SD card does not start",
	  "Open Settings. Under PS2 GAMES FROM THE SD CARD it must say Ready.\n"
	  "If it says it needs a one-time setup, choose Set up PS2 play, then quit Prism and open it again.\n"
	  "If it says Not on this console: PS2 games from the card work only on the early PS3 models that "
	  "play PS2 discs, with Cobra 8.3 or later.\n"
	  "Prism was tested on firmware 4.93 only. On an older one it says so, and a PS2 game may not start.\n"
	  "A PS2 game on the internal drive does not need the setup." },
	{ "Prism asks for the disc to be put in again",
	  "The console lets Prism read a PS1 or PS2 disc only when the disc goes in while Prism is open.\n"
	  "Take the disc out, wait for the drive to go quiet, put it back and wait. Prism goes on by itself.\n"
	  "If Prism says the console would not let it read the disc: quit Prism, restart the console, open "
	  "Prism, then put the disc in and try again.\n"
	  "If a game was played just before, Prism puts that game away first. That is normal." },
	{ "How long a copy takes",
	  "A PS1 disc: about 5 minutes. A PS2 disc: 10 to 15 minutes. A PS3 disc: from a few minutes to "
	  "an hour, by the size of the game.\n"
	  "Copying a game from one storage device to another takes about as long.\n"
	  "Circle stops a copy. A copy that was stopped, or that failed, leaves nothing behind.\n"
	  "If Prism says some of a disc could not be read, clean the disc and copy it again." },
	{ "Where a copy can go",
	  "Prism lists every storage device it finds, with its free space: the console's internal drive, "
	  "USB storage (a flash drive, a hard disk, an SSD) and cards in the console's card slots. A "
	  "device that cannot hold the game is shown with the reason.\n"
	  "PS3 games: the internal drive or USB storage. They do not start from a card slot.\n"
	  "PS2 games: the internal drive or the SD card.\n"
	  "PS1 games: any of them.\n"
	  "Most USB storage and cards cannot hold one file over 4 GB. A PS3 game with such a file goes to "
	  "the internal drive. A PS2 game that big is written to the SD card in parts, and Prism plays "
	  "the parts as one game." },
	{ "What a copy is made of",
	  "A PS1 copy is a .bin file with a small .cue file beside it. Keep the two together.\n"
	  "A PS2 copy is one .iso file, or parts named .iso.0, .iso.1 and so on. Keep the parts together.\n"
	  "A PS3 copy is a folder named after the game's code, inside a folder called GAMES.\n"
	  "Square on a game in a list copies it to another storage device, or deletes it." },
	{ "A game is missing from the list",
	  "Press SELECT on a games list to look again.\n"
	  "Prism looks on the internal drive, on USB storage and on cards in the console's card slots, in "
	  "these folders: PSXISO for PS1 games, PS2ISO for PS2 games (and PS2PRISM on the SD card), PS3ISO "
	  "and GAMES for PS3 games, PSPISO for PSP games.\n"
	  "PS2 games are read from the SD card only, and only on a console set up for it (the first help "
	  "page)." },
	{ "A game has a code for a name",
	  "Every game disc carries a code, like SLUS_214.47. Prism finds the game's name from that code in "
	  "its own list of titles.\n"
	  "A game that is not in the list is shown by its code. It plays all the same.\n"
	  "The code is also part of the file name of every copy Prism makes, so that two discs with the "
	  "same name can sit side by side." },
	{ "A PS1, PS3 or PSP game does not start",
	  "These start through webMAN MOD. In Settings, the webMAN MOD line must say answering.\n"
	  "If it says Auto-Play is on, choose Turn webMAN's Auto-Play off in Settings.\n"
	  "After you press X the XMB comes back and the game starts by itself a few seconds later. If it "
	  "does not, press X on the disc icon on the XMB.\n"
	  "A PSP game starts from PSP Launcher in the XMB's Game column." },
	/* v0.67-v0.70 */
	{ "Going on with the last game",
	  "On the home screen, Triangle starts the game you played last from Prism. The line above the "
	  "buttons says which game that is.\n"
	  "A game's page shows how many times it was started from Prism, and how long ago.\n"
	  "Only starts from Prism are counted, not starts from the XMB." },
	{ "Copying several games at once",
	  "In a games list, R2 marks the game you are on; a tick appears on its cover. Mark as many as you "
	  "like, then press Square and choose Copy the marked games.\n"
	  "Prism asks once where they should go and copies them one after another. Circle stops the "
	  "game being copied and the ones after it.\n"
	  "At the end it says how many were copied, and the first one that was not, with the reason.\n"
	  "PS2 games on a card go to the internal drive. PS2 games on the internal drive go to the SD card." },
	{ "Moving PS2 saves to another console or a PC",
	  "On the Memory Card screen, X on a save offers Send to, with every USB drive and card found. "
	  "The save is written there as a .psu file in a folder called PS2SAVES, and stays on the card too.\n"
	  "Square brings a .psu file from a PS2SAVES folder into the memory card on screen.\n"
	  ".psu files are what uLaunchELF on a PS2 and the PC tools for PCSX2 use, so a save can go to a "
	  "real PS2, to a PC, or to another PS3 with Prism.\n"
	  "Before a memory card is changed, Prism keeps a copy of it in /dev_hdd0/tmp/prism_vmc_bak." },
	{ "PSP games in PPSSPP",
	  "PPSSPP PS3 is another PSP emulator, made by other people, that some games run better in. It is "
	  "not part of Prism: install it yourself if you want it.\n"
	  "When it is installed, Square on a PSP game offers Open it in PPSSPP, and Settings can make it "
	  "the launcher for every PSP game.\n"
	  "PPSSPP build 314 or later starts the game at once. Settings, PPSSPP: opens its game list first, "
	  "makes it show its own list instead, as older builds always do: choose the game again there.\n"
	  "Quitting a game started this way goes back to the XMB. PPSSPP keeps its own saved games." },
	{ "The picture is wrong, or Prism closes by itself",
	  "Prism draws with the console's graphics chip. If the screen stays black or looks broken, quit "
	  "to the XMB and start Prism again while holding L1 and R1: it then draws the old, slower way.\n"
	  "If the graphics chip stops answering, Prism closes by itself and uses the old way from the next "
	  "start on.\n"
	  "/dev_hdd0/tmp/prism_gfx.log and prism_perf.log say what happened." },
	{ "The console stops answering",
	  "Wait one minute. If nothing changes, hold the power button until the console turns off, then "
	  "start it again.\n"
	  "Prism writes nothing to the console's own system memory. What it changes is on the internal "
	  "drive and the storage devices, and Settings can take its PS2 setup out again.\n"
	  "Prism keeps notes of what it did in /dev_hdd0/tmp, in files named prism_ and .log. They show "
	  "where it stopped." },
	{ "Removing Prism",
	  "First choose Remove the PS2 setup in Settings, if it is set up. That takes Prism's lines out of "
	  "/dev_hdd0/onxmb.bat and deletes its launcher image.\n"
	  "Then delete Prism from the XMB's Game column like any other game.\n"
	  "Your games, copies and memory cards stay where they are." },
	{ "Terms of use", terms_text },
};
#define N_TOPICS	((int)(sizeof topics / sizeof topics[0]))

static int page;

/* s drawn from (x, y) in lines no wider than w, dy apart; a '\n' starts a new paragraph (gap more).
 * Returns the y after the last line. */
static int wrap(int f, int x, int y, int w, int dy, int gap, col_t c, const char *s)
{
	char ln[200];

	while (*s)
	{
		int n = 0, last_fit = 0, k;

		/* the longest run of whole words that fits */
		for (k = 0; s[k] && s[k] != '\n' && k < (int)sizeof ln - 1; k++)
		{
			if (s[k] == ' ')
			{
				memcpy(ln, s, (size_t)k);
				ln[k] = 0;
				if (text_w(f, ln, 0) > w)
					break;
				last_fit = k;
			}
		}
		if (!s[k] || s[k] == '\n' || k == (int)sizeof ln - 1)
		{
			memcpy(ln, s, (size_t)k);
			ln[k] = 0;
			if (text_w(f, ln, 0) <= w || !last_fit)
				last_fit = k;
		}
		n = last_fit ? last_fit : k;
		memcpy(ln, s, (size_t)n);
		ln[n] = 0;
		text(f, x, y, c, ln);
		y += dy;
		s += n;
		while (*s == ' ')
			s++;
		if (*s == '\n')
		{
			s++;
			y += gap;
		}
	}
	return y;
}

unsigned help_nav_sig(void)	/* v0.72: screens.h */
{
	return (unsigned)page;
}

void help_open(void)
{
	page = 0;
}

void help_draw(void)
{
	char n[24];
	int x;

	top_bar("HELP");
	snprintf(n, sizeof n, "%d OF %d", page + 1, N_TOPICS);
	top_right(n, DIM, "", FAINT);
	text(F_BOLD, LEFT, 84, INK, topics[page].title);
	spectrum_bar((float)LEFT, (float)RIGHT, 110.0f, 1.0f, 6, 0x50);
	wrap(F_SMALL, LEFT, 124, RIGHT - LEFT, 18, 8, INK, topics[page].body);
	legend_line();
	x = legend_item(LEFT, -1, "LEFT RIGHT", "More help");
	legend_icon(x, 3, "Back");
}

/* 1 = back to SETTINGS */
int help_pad(unsigned pressed)
{
	if (pressed & PAD_CIRCLE)
		return 1;
	if (pressed & (PAD_RIGHT | PAD_R1 | PAD_DOWN | PAD_CROSS))
		page = page < N_TOPICS - 1 ? page + 1 : 0;
	if (pressed & (PAD_LEFT | PAD_L1 | PAD_UP))
		page = page ? page - 1 : N_TOPICS - 1;
	return 0;
}

/* ---- the terms, once per console ---- */

int terms_accepted(void)
{
	char ln[64];
	FILE *f = fopen(TERMS_FILE, "r");
	int v = 0;

	if (!f)
		return 0;
	if (fgets(ln, sizeof ln, f))
		sscanf(ln, "terms %d", &v);
	fclose(f);
	return v >= TERMS_VER;
}

void terms_accept(void)
{
	FILE *f = fopen(TERMS_FILE, "w");

	if (!f)
		return;
	fprintf(f, "terms %d agreed to on this console, Prism %s\n", TERMS_VER, APP_VERSION);
	fclose(f);
}

/* the first screen of a console that has not agreed yet. armed = X counts now. */
void terms_draw(int armed)
{
	int x, y;

	top_bar("BEFORE YOU START");
	text(F_BOLD, LEFT, 84, INK, "Terms of use");
	spectrum_bar((float)LEFT, (float)RIGHT, 110.0f, 1.0f, 6, 0x50);
	y = wrap(F_SMALL, LEFT, 124, RIGHT - LEFT, 18, 8, INK, terms_text);
	text(F_SMALL, LEFT, y + 10, AMBER, "Pressing X means you agree. Settings, Help shows these terms again.");
	legend_line();
	x = legend_item(LEFT, 0, NULL, armed ? "I agree" : "...");
	legend_item(x, 3, NULL, "Quit Prism");
}
