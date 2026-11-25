/* Stand-ins for the PC renderer (render.sh): v0.67's save files (src/savefiles.c needs the console): two
 * places and three files to draw the Memory Card screen's new sheets with. Nothing is read or written. */
#include <stdio.h>
#include <string.h>
#include "savefiles.h"

sf_dev_t sf_dev[SF_DEVS];
int n_sf_dev;
sf_file_t sf_file[SF_FILES];
int n_sf_file;

void sf_scan_devs(void)
{
	n_sf_dev = 2;
	strcpy(sf_dev[0].path, "/dev_usb000");
	strcpy(sf_dev[0].name, "USB 0");
	strcpy(sf_dev[1].path, "/dev_sd");
	strcpy(sf_dev[1].name, "SD card");
}

void sf_scan_files(void)
{
	static const struct { const char *save, *dev; unsigned files, kb; } f[3] = {
		{ "BASLUS-20946GTA50000", "USB 0", 6, 396 }, { "BASLUS-21359MGS3SNAKE", "USB 0", 4, 66 }, { "BASLUS-20202CZYTAXI", "SD card", 5, 360 },
	};
	int i;

	n_sf_file = 3;
	for (i = 0; i < 3; i++)
	{
		snprintf(sf_file[i].path, sizeof sf_file[i].path, "/stub/PS2SAVES/%s.psu", f[i].save);
		snprintf(sf_file[i].save, sizeof sf_file[i].save, "%s", f[i].save);
		snprintf(sf_file[i].dev, sizeof sf_file[i].dev, "%s", f[i].dev);
		sf_file[i].files = f[i].files;
		sf_file[i].kb = f[i].kb;
	}
}

int sf_exists(int d, const char *save)
{
	(void)save;
	return d == 0;
}

int sf_export(card_t *c, int si, int d, char *shown, int shown_n, char *why, int why_n)
{
	(void)c; (void)si; (void)d; (void)shown; (void)shown_n;
	snprintf(why, why_n, "the PC renderer writes nothing");
	return -1;
}

/* v0.73: the menu sounds' switch (src/sfx.c is console only) */
static int stub_menu_on = 1;
int menu_sounds_on(void) { return stub_menu_on; }
int menu_sounds_set(int on) { stub_menu_on = on; return 0; }
