/* v0.67: PS2 saves as files, to and from USB drives and card slots (the Memory Card screen, mcard.c).
 * A save goes out as <place>/PS2SAVES/<save folder>.psu and comes in from the same folder: the format
 * uLaunchELF, mymc and PCSX2's tools use (vmc.h vmc_export_psu / vmc_import_psu). This half finds the
 * places and the files; it needs the console (savefiles.c), the PC renderer has stand-ins. */
#ifndef PRISM_APP_SAVEFILES_H
#define PRISM_APP_SAVEFILES_H
#include "vmc.h"

#define SF_DIR		"PS2SAVES"
#define SF_DEVS		9	/* v0.75: was 6; a card's exFAT part can be a place too */
#define SF_FILES	64

typedef struct
{
	char path[24];		/* "/dev_usb000" */
	char name[20];		/* "USB 0" */
	int exf_slot;		/* v0.75: -1 = a place the PS3 mounts (path); else the card slot whose exFAT part this is
				 * (0 Memory Stick, 1 SD, 2 CompactFlash), read and written by Prism's own exFAT code */
} sf_dev_t;

typedef struct
{
	char path[200];
	char save[33];		/* the save's folder name inside the file */
	char dev[20];		/* the place's name */
	unsigned files, kb;
} sf_file_t;

extern sf_dev_t sf_dev[SF_DEVS];
extern int n_sf_dev;
extern sf_file_t sf_file[SF_FILES];
extern int n_sf_file;

/* the places a save file can go to or come from, as they are now: USB drives, the card slots that give
 * sound data (cardcheck.h), the hard disk */
void sf_scan_devs(void);
/* every .psu save file in the PS2SAVES folder of those places */
void sf_scan_files(void);
/* does place d already hold a file for this save? */
int  sf_exists(int d, const char *save);
/* write save si of card c to place d. The file is written under another name first and takes its real
 * name only when it is whole, so a file already there is lost only to a complete new one.
 * 0 = written (shown[] = "PS2SAVES/<name>.psu"); else why[] says why not */
int  sf_export(card_t *c, int si, int d, char *shown, int shown_n, char *why, int why_n);

#endif
