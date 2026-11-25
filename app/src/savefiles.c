/* v0.67: where PS2 save files go and come from (savefiles.h). Console only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lv2/sysfs.h>
#include <sys/file.h>	/* sysLv2FsRename */
#include "savefiles.h"
#include "dirlist.h"
#include "cardcheck.h"

sf_dev_t sf_dev[SF_DEVS];
int n_sf_dev;
sf_file_t sf_file[SF_FILES];
int n_sf_file;

static int is_dir(const char *p)
{
	sysFSStat st;

	return sysFsStat(p, &st) == 0 && (st.st_mode & 0170000) == 0040000;
}

static int is_file(const char *p)
{
	sysFSStat st;

	return sysFsStat(p, &st) == 0 && (st.st_mode & 0170000) != 0040000;
}

static void add_dev(const char *path, const char *name)
{
	int s = slot_of_path(path);

	if (n_sf_dev >= SF_DEVS || !is_dir(path))
		return;
	if (s >= 0 && slot_bad(s))
		return;		/* v0.65: a slot that gives back damaged data is neither read nor written */
	snprintf(sf_dev[n_sf_dev].path, sizeof sf_dev[0].path, "%s", path);
	snprintf(sf_dev[n_sf_dev].name, sizeof sf_dev[0].name, "%s", name);
	n_sf_dev++;
}

void sf_scan_devs(void)
{
	char p[24], n[20];
	int i;

	n_sf_dev = 0;
	for (i = 0; i < 8; i++)
	{
		snprintf(p, sizeof p, "/dev_usb%03d", i);
		snprintf(n, sizeof n, "USB %d", i);
		add_dev(p, n);
	}
	add_dev("/dev_sd", "SD card");
	add_dev("/dev_ms", "Memory Stick");
	add_dev("/dev_cf", "CompactFlash");
	add_dev("/dev_hdd0", "Hard disk");
}

static int ends_psu(const char *n)
{
	size_t l = strlen(n);

	return l > 4 && n[l - 4] == '.' && (n[l - 3] | 32) == 'p' && (n[l - 2] | 32) == 's' && (n[l - 1] | 32) == 'u';
}

void sf_scan_files(void)
{
	int d, i;

	n_sf_file = 0;
	for (d = 0; d < n_sf_dev; d++)
	{
		char dir[64];
		dl_ent *v = NULL;
		int n;

		snprintf(dir, sizeof dir, "%s/" SF_DIR, sf_dev[d].path);
		n = dir_list(dir, &v);
		for (i = 0; i < n && n_sf_file < SF_FILES; i++)
		{
			sf_file_t *f = &sf_file[n_sf_file];

			if (v[i].dir || !ends_psu(v[i].name) || strlen(v[i].name) > 120)
				continue;
			snprintf(f->path, sizeof f->path, "%s/%s", dir, v[i].name);
			if (vmc_psu_name(f->path, f->save, &f->files) != 0)
				continue;	/* named .psu, but not one */
			snprintf(f->dev, sizeof f->dev, "%s", sf_dev[d].name);
			f->kb = (unsigned)((v[i].size + 1023) / 1024);
			n_sf_file++;
		}
		free(v);
	}
}

static void target(int d, const char *save, char *path, int n)
{
	snprintf(path, n, "%s/" SF_DIR "/%s.psu", sf_dev[d].path, save);
}

int sf_exists(int d, const char *save)
{
	char p[200];

	if (d < 0 || d >= n_sf_dev)
		return 0;
	target(d, save, p, sizeof p);
	return is_file(p);
}

int sf_export(card_t *c, int si, int d, char *shown, int shown_n, char *why, int why_n)
{
	char dir[64], fin[200], tmp[210], nm[33];
	unsigned files = 0;

	why[0] = 0;
	if (d < 0 || d >= n_sf_dev || si < 0 || si >= c->n)
	{
		snprintf(why, why_n, "that place is not there any more");
		return -1;
	}
	if (slot_of_path(sf_dev[d].path) >= 0 && slot_bad(slot_of_path(sf_dev[d].path)))
	{
		snprintf(why, why_n, "that card slot gives back damaged data: nothing is written there");
		return -1;
	}
	snprintf(dir, sizeof dir, "%s/" SF_DIR, sf_dev[d].path);
	if (!is_dir(dir) && (sysFsMkdir(dir, 0777) != 0 || !is_dir(dir)))
	{
		snprintf(why, why_n, "the folder " SF_DIR " could not be made on %s", sf_dev[d].name);
		return -1;
	}
	target(d, c->saves[si].dir, fin, sizeof fin);
	snprintf(tmp, sizeof tmp, "%s.new", fin);
	if (vmc_export_psu(c, si, tmp, why, why_n))
		return -1;
	/* read back what the device holds now, before it takes the place of anything */
	if (vmc_psu_name(tmp, nm, &files) != 0 || strcmp(nm, c->saves[si].dir) || (int)files != c->saves[si].nfiles)
	{
		sysFsUnlink(tmp);
		snprintf(why, why_n, "the file read back from %s is not the save that was written", sf_dev[d].name);
		return -1;
	}
	if (is_file(fin))
		sysFsUnlink(fin);
	if (sysLv2FsRename(tmp, fin) != 0)
	{
		snprintf(why, why_n, "the file was written as %s.psu.new but could not be given its name", c->saves[si].dir);
		return -1;
	}
	snprintf(shown, shown_n, SF_DIR "/%s.psu", c->saves[si].dir);
	return 0;
}
