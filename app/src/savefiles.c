/* v0.67: where PS2 save files go and come from (savefiles.h). Console only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lv2/sysfs.h>
#include <sys/file.h>	/* sysLv2FsRename */
#include "savefiles.h"
#include "dirlist.h"
#include "cardcheck.h"
#include "cardcheck_core.h"	/* CC_OK */
#include "games.h"		/* card_dev */
#include "exfat_w.h"
#include "exfat_card.h"

/* ---- v0.75: a card that is exFAT only (Bananafuzzbuddy, 11-25: "there is no option to copy a memory card to the
 * memory stick"; theirs is exFAT, which the PS3 does not mount). Such a card is a place for save files
 * through Prism's own exFAT code (exfat_w.c: folder, list, read, write and delete, each judged on the PC by
 * fsck.exfat and exfat-fuse: host/exw_test.sh, exd_test.sh, exs_test.sh). The save code itself reads and
 * writes files by path, so a save file passes through /dev_hdd0/tmp/prism_psu on its way in or out.
 * Only a slot whose card passed the damage check (cardcheck.h), and only when the PS3 mounts nothing there
 * (a card with a FAT32 part is already a place by its path). ---- */
#define PSU_TMP		"/dev_hdd0/tmp/prism_psu"
#define PSU_MAX		(9u << 20)	/* a PS2 card holds 8 MB */

static int exf_open(int s, exw_dev *dv, char *why, int wn)
{
	u64 was = card_dev;
	int r;

	card_dev = slot_storage(s);	/* exc_open opens card_dev; it keeps its own handle afterwards */
	r = exc_open(dv, why, wn);
	card_dev = was;
	return r;
}

static int ends_psu(const char *n)
{
	size_t l = strlen(n);

	return l > 4 && n[l - 4] == '.' && (n[l - 3] | 32) == 'p' && (n[l - 2] | 32) == 's' && (n[l - 1] | 32) == 'u';
}

typedef struct { char name[64][124]; unsigned size[64]; int n; } exf_ls;

static int exf_ls_cb(void *arg, const char *name, uint64_t size, int is_dir)
{
	exf_ls *l = (exf_ls *)arg;

	if (is_dir || !ends_psu(name) || strlen(name) > 120 || size > PSU_MAX || l->n >= 64)
		return 0;
	snprintf(l->name[l->n], sizeof l->name[0], "%s", name);
	l->size[l->n] = (unsigned)size;
	l->n++;
	return 0;
}

static int same_nocase(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if ((*a | 32) != (*b | 32) && *a != *b)
			return 0;
	return !*a && !*b;
}

/* sf_exists is asked every frame while a question is up: the card is asked once, then remembered */
static int ex_d = -1, ex_ans;
static char ex_save[40];

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
	sf_dev[n_sf_dev].exf_slot = -1;
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
	{
		/* v0.75: a card the PS3 does not mount, with an exFAT volume Prism can write */
		static const char *const mnt[3] = { "/dev_ms", "/dev_sd", "/dev_cf" };
		static const char *const nm[3] = { "Memory Stick", "SD card", "CompactFlash" };
		int s;

		ex_d = -1;
		for (s = 0; s < 3 && n_sf_dev < SF_DEVS; s++)
		{
			exw_dev dv;
			uint64_t fr = 0, tot = 0;
			char why[120];
			int ok;

			if (slot_cc[s] != CC_OK || is_dir(mnt[s]))
				continue;
			if (exf_open(s, &dv, why, sizeof why))
				continue;
			ok = exw_space(&dv, &fr, &tot, why, sizeof why) == 0;
			exc_close();
			if (!ok)
				continue;
			snprintf(sf_dev[n_sf_dev].path, sizeof sf_dev[0].path, "exfat:%d", s);
			snprintf(sf_dev[n_sf_dev].name, sizeof sf_dev[0].name, "%s", nm[s]);
			sf_dev[n_sf_dev].exf_slot = s;
			n_sf_dev++;
		}
	}
}


void sf_scan_files(void)
{
	int d, i;

	n_sf_file = 0;
	{
		/* v0.75: the copies of an exFAT card's save files from the last look go */
		dl_ent *old = NULL;
		int k, m = dir_list(PSU_TMP, &old);

		for (k = 0; k < m; k++)
		{
			char p[300];

			snprintf(p, sizeof p, PSU_TMP "/%s", old[k].name);
			if (!old[k].dir)
				sysFsUnlink(p);
		}
		free(old);
	}
	for (d = 0; d < n_sf_dev; d++)
	{
		char dir[64];
		dl_ent *v = NULL;
		int n;

		if (sf_dev[d].exf_slot >= 0)
		{
			/* v0.75: each .psu in the card's PS2SAVES is read whole and kept on the hard disk for this look */
			static exf_ls ls;
			exw_dev dv;
			char why[120];
			u8 *buf = (u8 *)malloc(PSU_MAX);
			int k;

			ls.n = 0;
			if (buf && exf_open(sf_dev[d].exf_slot, &dv, why, sizeof why) == 0)
			{
				exw_list(&dv, SF_DIR, exf_ls_cb, &ls, why, sizeof why);
				sysFsMkdir(PSU_TMP, 0777);
				for (k = 0; k < ls.n && n_sf_file < SF_FILES; k++)
				{
					sf_file_t *f = &sf_file[n_sf_file];
					uint32_t got = 0;
					FILE *o;

					if (exw_read(&dv, SF_DIR, ls.name[k], buf, PSU_MAX, &got, why, sizeof why) || got != ls.size[k])
						continue;
					snprintf(f->path, sizeof f->path, PSU_TMP "/%d_%s", sf_dev[d].exf_slot, ls.name[k]);
					o = fopen(f->path, "wb");
					if (!o)
						continue;
					if (fwrite(buf, 1, got, o) != got)
					{
						fclose(o);
						continue;
					}
					if (fclose(o) != 0 || vmc_psu_name(f->path, f->save, &f->files) != 0)
						continue;	/* named .psu, but not one */
					snprintf(f->dev, sizeof f->dev, "%s", sf_dev[d].name);
					f->kb = (ls.size[k] + 1023) / 1024;
					n_sf_file++;
				}
				exc_close();
			}
			free(buf);
			continue;
		}

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
	if (sf_dev[d].exf_slot >= 0)
	{
		static exf_ls ls;
		exw_dev dv;
		char why[120], want[48];
		int k;

		if (ex_d == d && !strcmp(ex_save, save))
			return ex_ans;
		ex_d = d;
		snprintf(ex_save, sizeof ex_save, "%s", save);
		ex_ans = 0;
		snprintf(want, sizeof want, "%s.psu", save);
		ls.n = 0;
		if (exf_open(sf_dev[d].exf_slot, &dv, why, sizeof why) == 0)
		{
			exw_list(&dv, SF_DIR, exf_ls_cb, &ls, why, sizeof why);
			exc_close();
		}
		for (k = 0; k < ls.n; k++)
			if (same_nocase(ls.name[k], want))
				ex_ans = 1;
		return ex_ans;
	}
	target(d, save, p, sizeof p);
	return is_file(p);
}

/* v0.75: save si of card c onto the exFAT part of slot s. Written to the hard disk first (the save code
 * writes by path), then put on the card, then read back from the card and compared byte for byte. A file of
 * that name already there is deleted first: the save itself stays on the memory card whatever happens. */
static int exf_export(card_t *c, int si, int d, char *shown, int shown_n, char *why, int why_n)
{
	static exf_ls ls;
	const char *tmp = PSU_TMP "/out.psu";
	char nm[33], name[48];
	unsigned files = 0;
	exw_dev dv;
	sysFSStat st;
	u8 *buf = NULL, *back = NULL;
	uint32_t got = 0;
	FILE *f;
	int r = -1, k, there = 0;

	ex_d = -1;
	sysFsMkdir(PSU_TMP, 0777);
	if (vmc_export_psu(c, si, tmp, why, why_n))
		return -1;
	if (vmc_psu_name(tmp, nm, &files) != 0 || strcmp(nm, c->saves[si].dir) || (int)files != c->saves[si].nfiles
		|| sysFsStat(tmp, &st) != 0 || st.st_size == 0 || st.st_size > PSU_MAX)
	{
		sysFsUnlink(tmp);
		snprintf(why, why_n, "the save file made on the hard disk is not the save");
		return -1;
	}
	buf = (u8 *)malloc((size_t)st.st_size);
	back = (u8 *)malloc((size_t)st.st_size);
	f = fopen(tmp, "rb");
	if (!buf || !back || !f || fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size)
	{
		snprintf(why, why_n, "could not read the save file back from the hard disk");
		goto out;
	}
	snprintf(name, sizeof name, "%s.psu", c->saves[si].dir);
	if (exf_open(sf_dev[d].exf_slot, &dv, why, why_n))
		goto out;
	ls.n = 0;
	if (exw_list(&dv, SF_DIR, exf_ls_cb, &ls, why, why_n) != 0)
	{
		/* no PS2SAVES folder yet (or the volume cannot be used: then making it says why) */
		if (exw_mkdir(&dv, SF_DIR, why, why_n) != 0)
			goto close;
	}
	for (k = 0; k < ls.n; k++)
		if (same_nocase(ls.name[k], name))
			there = 1;
	if (there && exw_delete(&dv, SF_DIR, name, why, why_n) != 0)
		goto close;
	if (exw_begin(&dv, SF_DIR, name, (uint64_t)st.st_size, why, why_n) != 0)
		goto close;
	if (exw_write(buf, (uint32_t)st.st_size) != 0)
	{
		exw_abort();
		snprintf(why, why_n, "writing to the %s stopped: nothing was added to it", sf_dev[d].name);
		goto close;
	}
	if (exw_finish(why, why_n) != 0)
		goto close;
	if (exw_read(&dv, SF_DIR, name, back, (uint32_t)st.st_size, &got, why, why_n) != 0 || got != (uint32_t)st.st_size
		|| memcmp(buf, back, (size_t)st.st_size))
	{
		char w2[80];

		exw_delete(&dv, SF_DIR, name, w2, sizeof w2);
		snprintf(why, why_n, "the file read back from the %s is not the save that was written: it was taken off again", sf_dev[d].name);
		goto close;
	}
	snprintf(shown, shown_n, SF_DIR "/%s", name);
	r = 0;
close:
	exc_close();
out:
	if (f)
		fclose(f);
	free(buf);
	free(back);
	sysFsUnlink(tmp);
	return r;
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
	if (sf_dev[d].exf_slot >= 0)
		return exf_export(c, si, d, shown, shown_n, why, why_n);
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
