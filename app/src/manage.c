/* Deleting games, the disc in the drive and disc shortcuts (v0.15).
 *
 * Delete removes exactly the files of one game, after two questions (screens.c):
 *   PS1   the image, its .cue, and the files that .cue names (same folder)
 *   PS3   an .iso, or every part of a split .iso.0 / .iso.1 ...; a game folder in GAMES, whole
 *   PS2   only from the card's FAT32 part the PS3 mounts as /dev_sd (/PS2ISO or /PS2PRISM, whole or
 *         split name.0 name.1 ...): the exFAT part is read by Prism's own reader, and the PS3
 *         cannot write it.
 * Nothing is deleted while webMAN has the game mounted.
 *
 * The disc: a PS3 game disc in the drive shows up in the PS3 list (a PS1 / PS2 disc too, when
 * GameOS lets an app read it), and "Add to this list" keeps it there as a shortcut when the
 * disc is out. Shortcuts live in USRDIR/shortcuts.txt, one "<platform>\t<title ID>\t<title>" a line. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <lv2/sysfs.h>
#include "manage.h"
#include "webman.h"
#include "screens.h"
#include "dirlist.h"
#include "cardcheck.h"	/* v0.65 */
#include "exfat_w.h"	/* v0.73 */
#include "exfat_card.h"
#include "version.h"

extern u32 exc_writes, exc_errs;	/* exfat_card.c */

int app_events(void);	/* main.c: the system's events; 1 = asked to quit */

#define SHORTCUTS	"/dev_hdd0/game/PRSM00001/USRDIR/shortcuts.txt"
#define BD		"/dev_bdvd"
#define MAX_DEL		600

static char del_list[MAX_DEL][256];
static int n_del, del_dirs;
static u64 del_bytes;
static int del_exfat;	/* v0.73: the game is one file on a card's exFAT part: deleted by Prism's own exFAT writer */

/* ---- webMAN: is a game mounted over the drive? ---- */

static char wm_page[12 * 1024];

/* The status page says '<a href="/dev_bdvd">/dev_bdvd</a> -> <the mounted path>' only while
 * something is mounted; "/dev_bdvd" alone is always on it (a link at the bottom), and the line
 * sits ~5.5 KB in (v0.12-v0.14 read 2 KB and looked for "/dev_bdvd": never saw a mount). */
int wm_mounted(char *what, int n)
{
	char *p, *q;
	int k = 0;

	if (what && n)
		what[0] = 0;
	if (wm_get_body("/cpursx.ps3", wm_page, sizeof wm_page) <= 0)
		return 0;
	p = strstr(wm_page, "/dev_bdvd</a> -> ");
	if (!p)
		return 0;
	p += 17;
	if (what)
	{
		/* the path, without the links' tags */
		for (q = p; *q && k < n - 1 && strncmp(q, "<hr", 3) && strncmp(q, "</font", 6); q++)
		{
			if (*q == '<')
			{
				while (*q && *q != '>')
					q++;
				if (!*q)
					break;
				continue;
			}
			what[k++] = *q;
		}
		what[k] = 0;
	}
	return 1;
}

/* ---- small things ---- */

static int exists(const char *p)
{
	sysFSStat st;

	return sysFsStat(p, &st) == 0;
}

static u64 fsize(const char *p)
{
	sysFSStat st;

	return sysFsStat(p, &st) == 0 ? st.st_size : 0;
}

static int is_dir(const char *p)
{
	sysFSStat st;

	return sysFsStat(p, &st) == 0 && (st.st_mode & 0170000) == 0040000;
}

static void add_file(const char *p)
{
	int i;

	for (i = 0; i < n_del; i++)
		if (!strcmp(del_list[i], p))
			return;
	if (n_del < MAX_DEL && exists(p))
	{
		snprintf(del_list[n_del++], sizeof del_list[0], "%s", p);
		del_bytes += fsize(p);
	}
}

static void fmt_size(char *o, int n, u64 b)
{
	if (b >= (1ull << 30))
		snprintf(o, n, "%.1f GB", b / 1073741824.0);
	else if (b >= (1ull << 20))
		snprintf(o, n, "%llu MB", (unsigned long long)(b >> 20));
	else
		snprintf(o, n, "%llu KB", (unsigned long long)((b + 1023) >> 10));
}

static const char *dev_word(const char *path)
{
	if (!strncmp(path, "/dev_sd", 7))
		return "the SD card";
	if (!strncmp(path, "/dev_hdd0", 9))
		return "the hard disk";
	if (!strncmp(path, "/dev_usb", 8))
		return "a USB drive";
	if (!strncmp(path, "/dev_ms", 7))
		return "the Memory Stick";
	if (!strncmp(path, "/dev_cf", 7))
		return "the CompactFlash card";
	return "that device";
}

/* the folder a path is in, and its file name */
static void split_path(const char *p, char *dir, int dn, const char **name)
{
	const char *s = strrchr(p, '/');
	int k = s ? (int)(s - p) : 0;

	snprintf(dir, dn, "%.*s", k, p);
	*name = s ? s + 1 : p;
}

/* a PS1 .cue: every FILE "x" it names, in its folder */
static void cue_files(const char *cue, const char *dir)
{
	char b[4096], *p;
	FILE *f = fopen(cue, "rb");
	int n;

	if (!f)
		return;
	n = (int)fread(b, 1, sizeof b - 1, f);
	fclose(f);
	b[n < 0 ? 0 : n] = 0;
	for (p = b; (p = strstr(p, "FILE ")) != NULL; p += 5)
	{
		char nm[200], full[256], *q = p + 5;
		int k = 0;

		if (*q == '"')
		{
			q++;
			while (*q && *q != '"' && k < (int)sizeof nm - 1)
				nm[k++] = *q++;
		}
		else
			while (*q > ' ' && k < (int)sizeof nm - 1)
				nm[k++] = *q++;
		nm[k] = 0;
		if (!k || strchr(nm, '/') || strstr(nm, ".."))
			continue;
		snprintf(full, sizeof full, "%s/%s", dir, nm);
		add_file(full);
	}
}

static int folder_ok(const char *p)
{
	/* <device>/GAMES/<one name>: nothing else is ever deleted as a tree */
	static const char *const dev[] = { "/dev_sd/GAMES/", "/dev_hdd0/GAMES/", "/dev_usb00", "/dev_ms/GAMES/", "/dev_cf/GAMES/", NULL };
	const char *name;
	int i;

	for (i = 0; dev[i]; i++)
	{
		if (strncmp(p, dev[i], strlen(dev[i])))
			continue;
		if (i == 2)
		{
			if (strlen(p) < 18 || strncmp(p + 11, "/GAMES/", 7))
				return 0;
			name = p + 18;
		}
		else
			name = p + strlen(dev[i]);
		return name[0] && !strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..");
	}
	return 0;
}

static u64 tb(void)
{
	u64 t;

	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

#define TB_HZ	79800000ULL
static u64 t_drawn;
static const char *job_title;

/* redraw at most 4 times a second (a frame costs ~40 ms of PPU) and let the system's events in */
static void tick(const char *head, float done, const char *what, int force)
{
	if (force || tb() - t_drawn > TB_HZ / 4)
	{
		progress(NULL, head, job_title, done, what);
		t_drawn = tb();
	}
	app_events();
}

static void walk(const char *dir, int depth)
{
	dl_ent *v;
	int k, m;

	if (depth > 24)
		return;
	m = dir_list(dir, &v);
	for (k = 0; k < m; k++)
	{
		char p[512];

		snprintf(p, sizeof p, "%s/%s", dir, v[k].name);
		if (v[k].dir)
		{
			del_dirs++;
			walk(p, depth + 1);
		}
		else
		{
			char ln[80];

			del_bytes += v[k].size;
			n_del++;	/* a tree is counted, not listed: rm_tree lists it again */
			snprintf(ln, sizeof ln, "%d files so far", n_del);
			tick("FINDING ITS FILES", 0.3f, ln, 0);
		}
	}
	free(v);
}

/* ---- delete ---- */

static int plan(int plat, const game_t *g, char *why, int n)
{
	char dir[256];
	const char *name;
	int i;

	n_del = del_dirs = 0;
	del_bytes = 0;
	del_exfat = 0;
	if (g->kind == 4 || g->kind == 5)
	{
		snprintf(why, n, "That is the disc (or its shortcut): nothing on a storage device to delete");
		return -1;
	}
	if (plat == P_PS3 && g->exfat)
	{
		/* v0.82: a PS3 ISO on a card's exFAT part (games.c scan_ps3_cards): GameOS cannot see it, so none of the file
		 * steps below would find it. Played from the card only, for now */
		snprintf(why, n, "Not yet: a PS3 game on the card's exFAT part is only played from there. Delete or copy it on a PC");
		return -9;
	}
	if (plat == P_PS2 && g->where == 2)
	{
		/* v0.16: a PS2 image on the hard disk (/dev_hdd0/PS2ISO), e.g. the Tiny Tees Prism brings */
		char p[300];
		const char *dot = strrchr(g->path, '.');
		int k = dot ? (int)(dot - g->path) : (int)strlen(g->path);
		static const char *const ext[] = { ".png", ".PNG", ".jpg", ".JPG" };

		if (strncmp(g->path, "/dev_hdd0/PS2ISO/", 17) || strchr(g->path + 17, '/'))
		{
			snprintf(why, n, "Not a PS2 image Prism deletes (%s)", g->path);
			return -6;
		}
		add_file(g->path);
		snprintf(p, sizeof p, "%s.CONFIG", g->path);
		add_file(p);
		for (i = 0; i < 4; i++)
		{
			snprintf(p, sizeof p, "%.*s%s", k, g->path, ext[i]);
			add_file(p);
		}
		return n_del ? 0 : -7;
	}
	if (plat == P_PS2 && g->where >= 3 && g->where <= 10)
	{
		/* v0.78: a PS2 image kept on a USB drive: /dev_usbNNN/PS2ISO/<one name>, with what came along with it */
		char p[300];
		const char *dot = strrchr(g->path, '.');
		int k = dot ? (int)(dot - g->path) : (int)strlen(g->path);
		static const char *const ext[] = { ".png", ".PNG", ".jpg", ".JPG" };

		if (strlen(g->path) < 20 || strncmp(g->path, "/dev_usb00", 10) || strncmp(g->path + 11, "/PS2ISO/", 8) || strchr(g->path + 19, '/'))
		{
			snprintf(why, n, "Not a PS2 image Prism deletes (%s)", g->path);
			return -6;
		}
		add_file(g->path);
		snprintf(p, sizeof p, "%s.CONFIG", g->path);
		add_file(p);
		for (i = 0; i < 4; i++)
		{
			snprintf(p, sizeof p, "%.*s%s", k, g->path, ext[i]);
			add_file(p);
		}
		if (!n_del)
			snprintf(why, n, "Its file was not found on the USB drive");
		return n_del ? 0 : -7;
	}
	if (plat == P_PS2 && g->where == 1 && g->exfat)
	{
		/* v0.73 (Bananafuzzbuddy, 11-25: "if a game is in an exfat partition I can't delete it? ... i obviously can see
		 * it and transfer it"). GameOS cannot change exFAT; Prism's own writer can take a file out
		 * (exfat_w.c exw_delete, judged on the PC by fsck.exfat and exfat-fuse: host/exd_test.sh). */
		del_exfat = 1;
		n_del = 1;
		del_bytes = g->size;
		return 0;
	}
	if (plat == P_PS2)
	{
		/* v0.73: on the game's OWN card. Up to v0.72 this looked on /dev_sd whatever slot the game was in,
		 * so a game on the Memory Stick could have deleted the SD card's file of the same name. */
		static const char *const slot_dev_[3] = { "/dev_ms", "/dev_sd", "/dev_cf" };
		static const char *const subs[] = { "PS2ISO", "PS2PRISM" };
		char dirs[2][40];
		int d;

		for (d = 0; d < 2; d++)
			snprintf(dirs[d], sizeof dirs[d], "%s/%s", slot_dev_[g->where == 1 && g->slot < 3 ? g->slot : 1], subs[d]);
		for (d = 0; d < 2 && !n_del; d++)
		{
			char p[300];

			snprintf(p, sizeof p, "%s/%s", dirs[d], g->name);
			if (exists(p) && !is_dir(p))
				add_file(p);
			else
				for (i = 0; i < 16; i++)
				{
					snprintf(p, sizeof p, "%s/%s.%d", dirs[d], g->name, i);
					if (!exists(p))
						break;
					add_file(p);
				}
		}
		if (!n_del)
		{
			snprintf(why, n, "Its file was not found on the card");
			return -2;
		}
		return 0;
	}
	if (plat == P_PS3 && g->kind == 3)
	{
		if (!folder_ok(g->path) || !exists(g->path))
		{
			snprintf(why, n, "Not a game folder Prism deletes (%s)", g->path);
			return -3;
		}
		walk(g->path, 0);
		return 0;
	}
	split_path(g->path, dir, sizeof dir, &name);
	if (plat == P_PSP)
	{
		char p[300];
		const char *dot = strrchr(g->path, '.');
		int k = dot ? (int)(dot - g->path) : (int)strlen(g->path);
		static const char *const ext[] = { ".png", ".PNG", ".jpg", ".JPG" };

		add_file(g->path);
		for (i = 0; i < 4; i++)
		{
			snprintf(p, sizeof p, "%.*s%s", k, g->path, ext[i]);
			add_file(p);
		}
		return n_del ? 0 : -8;
	}
	if (plat == P_PS3)
	{
		size_t L = strlen(g->path);

		if (L > 6 && !strcmp(g->path + L - 2, ".0"))
		{
			char base[256];

			snprintf(base, sizeof base, "%.*s", (int)(L - 2), g->path);
			for (i = 0; i < 64; i++)
			{
				char p[300];

				snprintf(p, sizeof p, "%s.%d", base, i);
				if (!exists(p))
					break;
				add_file(p);
			}
		}
		else
			add_file(g->path);
		return n_del ? 0 : -4;
	}
	/* PS1: the image, its .cue (same name), what the .cue names */
	add_file(g->path);
	{
		char cue[300];
		const char *dot = strrchr(g->path, '.');
		int k = dot ? (int)(dot - g->path) : (int)strlen(g->path);
		static const char *const ext[] = { ".cue", ".CUE", ".Cue" };

		for (i = 0; i < 3; i++)
		{
			snprintf(cue, sizeof cue, "%.*s%s", k, g->path, ext[i]);
			if (exists(cue))
			{
				add_file(cue);
				cue_files(cue, dir);
				break;
			}
		}
	}
	return n_del ? 0 : -5;
}

static int busy(const game_t *g, char *why, int n)
{
	char m[300];

	if (wm_mounted(m, sizeof m) && (strstr(m, g->path) || strstr(g->path, m)))
	{
		snprintf(why, n, "It is mounted by webMAN right now: unmount it on the XMB first");
		return 1;
	}
	return 0;
}

unsigned long long game_del_bytes(void)	/* v0.75: the size of what the last game_del_plan found */
{
	return del_bytes;
}

int game_del_plan(int plat, const game_t *g, char *what, int n)
{
	char sz[24];
	int r;

	if (busy(g, what, n))
		return -9;
	if (slot_bad_msg(slot_of_game(g), "Not deleted", what, n))	/* v0.65: the system would write the slot's damage back */
		return -8;
	job_title = g->shown;
	t_drawn = 0;
	tick("FINDING ITS FILES", 0.2f, "", 1);
	r = plan(plat, g, what, n);
	if (r)
		return r;
	fmt_size(sz, sizeof sz, del_bytes);
	if (del_exfat)
		snprintf(what, n, "1 file, %s, from the %s card's exFAT part", sz, slot_name(g->slot));
	else if (del_dirs)
		snprintf(what, n, "%d files in %d folders, %s, from %s", n_del, del_dirs + 1, sz, dev_word(g->path));
	else
		snprintf(what, n, "%d file%s, %s, from %s", n_del, n_del == 1 ? "" : "s", sz, dev_word(g->path));
	return 0;
}

static int gone, failed, stopped;
static unsigned (*del_pad)(void);
static unsigned pad_was;

static int stop_asked(void)
{
	unsigned b = del_pad ? del_pad() : 0, p = b & ~pad_was;

	pad_was = b;
	return (p & PAD_CIRCLE) || app_events();
}

static void deleted_one(const char *p)
{
	char ln[200];

	gone++;
	snprintf(ln, sizeof ln, "%d of %d files  " DOT "  Circle stops", gone, n_del);
	tick("DELETING", n_del ? (float)gone / n_del : 0.5f, ln, 0);
	(void)p;
}

static void rm_tree(const char *dir, int depth)
{
	dl_ent *v;
	int k, m;

	if (depth > 24 || stopped)
		return;
	m = dir_list(dir, &v);
	for (k = 0; k < m && !stopped; k++)
	{
		char p[512];

		snprintf(p, sizeof p, "%s/%s", dir, v[k].name);
		if (v[k].dir)
			rm_tree(p, depth + 1);
		else if (sysFsUnlink(p) == 0)
			deleted_one(p);
		else
			failed++;
		if (stop_asked())
			stopped = 1;
	}
	free(v);
	if (!stopped && sysFsRmdir(dir) != 0)
		failed++;
}

/* v0.73: one game file on a card's exFAT part. The card's slot passed the damage check (the caller's
 * slot_bad_msg); the writer checks the volume and the file before its first write and says why it will not.
 * A game kept in parts (name.0, name.1 ...) goes part by part. */
static int exfat_delete(const game_t *g, char *msg, int n)
{
	static const char *const subs[] = { "PS2ISO", "PS2PRISM" };
	u64 dev_was = card_dev;
	exw_dev dv;
	char why[200], part[130];
	FILE *lg = fopen("/dev_hdd0/tmp/prism_rip.log", "a");
	int r = -1, d, k, parts = 0;

	why[0] = 0;
	card_dev = slot_storage(g->slot);	/* exc_open opens card_dev: this game's slot, not the first card read */
	if (exc_open(&dv, why, sizeof why) == 0)
	{
		for (d = 0; d < 2 && r == -1; d++)
			r = exw_delete(&dv, subs[d], g->name, why, sizeof why);
		if (lg)
			fprintf(lg, "=== delete from exFAT, app v%s: slot %s, %s: %d (%s), %u writes, %u errors\n", APP_VERSION, slot_name(g->slot), g->name, r, why,
				(unsigned)exc_writes, (unsigned)exc_errs);
		if (r == -1 && !strcmp(why, "no file with that name is there"))
		{
			/* in parts */
			for (d = 0; d < 2 && !parts; d++)
				for (k = 0; k < 64; k++)
				{
					snprintf(part, sizeof part, "%s.%d", g->name, k);
					r = exw_delete(&dv, subs[d], part, why, sizeof why);
					if (lg)
						fprintf(lg, "  part %s/%s: %d (%s)\n", subs[d], part, r, why);
					if (r)
						break;
					parts++;
				}
			if (parts && r == -1 && !strcmp(why, "no file with that name is there"))
				r = 0;		/* the part after the last */
		}
		exc_close();
	}
	card_dev = dev_was;
	if (lg)
		fclose(lg);
	if (r == 0)
		snprintf(msg, n, "Deleted %s.", g->shown);
	else if (r == -2 || parts)
		snprintf(msg, n, "%s is gone, but not cleanly: %s.", g->shown, why);
	else
		snprintf(msg, n, "Not deleted: %s.", why);
	return r;
}

int game_delete(unsigned (*pad)(void), int plat, const game_t *g, char *msg, int n)
{
	char why[200];
	int i;

	if (busy(g, msg, n))
		return -9;
	if (slot_bad_msg(slot_of_game(g), "Not deleted", msg, n))
		return -8;
	job_title = g->shown;
	t_drawn = 0;
	tick("FINDING ITS FILES", 0.2f, "", 1);
	if (plan(plat, g, why, sizeof why))
	{
		snprintf(msg, n, "Not deleted: %s.", why);
		return -1;
	}
	gone = failed = stopped = 0;
	del_pad = pad;
	pad_was = pad ? pad() : 0;	/* the Cross that said "Delete" is not a Circle */
	tick("DELETING", 0.0f, "starting", 1);
	if (del_exfat)
		return exfat_delete(g, msg, n);
	if (plat == P_PS3 && g->kind == 3)
		rm_tree(g->path, 0);
	else
		for (i = 0; i < n_del && !stopped; i++)
		{
			if (sysFsUnlink(del_list[i]) == 0)
				deleted_one(del_list[i]);
			else
				failed++;
			if (stop_asked())
				stopped = 1;
		}
	if (stopped)
		snprintf(msg, n, "Stopped after %d of %d files: the rest of %s is still there.", gone, n_del, g->shown);
	else if (failed)
		snprintf(msg, n, "Deleted %d file%s; %d could not be deleted.", gone, gone == 1 ? "" : "s", failed);
	else
		snprintf(msg, n, "Deleted %s.", g->shown);
	return failed || stopped ? -2 : 0;
}

/* ---- shortcuts ---- */

typedef struct
{
	int plat;
	char id[12], title[100];
} sc_t;

static sc_t sc[64];
static int n_sc;

static void sc_load(void)
{
	static char b[16 * 1024];
	char *p, *line;
	FILE *f = fopen(SHORTCUTS, "rb");
	int n;

	n_sc = 0;
	if (!f)
		return;
	n = (int)fread(b, 1, sizeof b - 1, f);
	fclose(f);
	b[n < 0 ? 0 : n] = 0;
	for (line = b; line && *line && n_sc < 64; line = p)
	{
		char *t1, *t2;

		p = strchr(line, '\n');
		if (p)
			*p++ = 0;
		t1 = strchr(line, '\t');
		t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
		if (!t1 || !t2 || line[0] < '0' || line[0] > '2')
			continue;
		*t1 = *t2 = 0;
		sc[n_sc].plat = line[0] - '0';
		snprintf(sc[n_sc].id, sizeof sc[n_sc].id, "%s", t1 + 1);
		snprintf(sc[n_sc].title, sizeof sc[n_sc].title, "%s", t2 + 1);
		if (sc[n_sc].id[0])
			n_sc++;
	}
}

static int sc_save(void)
{
	FILE *f = fopen(SHORTCUTS, "wb");
	int i;

	if (!f)
		return -1;
	for (i = 0; i < n_sc; i++)
		fprintf(f, "%d\t%s\t%s\n", sc[i].plat, sc[i].id, sc[i].title);
	return fclose(f) == 0 ? 0 : -2;
}

static int sc_find(int plat, const char *id)
{
	int i;

	for (i = 0; i < n_sc; i++)
		if (sc[i].plat == plat && !strcmp(sc[i].id, id))
			return i;
	return -1;
}

int shortcut_add(int plat, const game_t *g, char *msg, int n)
{
	sc_load();
	if (!g->id[0])
	{
		snprintf(msg, n, "Not added: the disc has no title ID.");
		return -1;
	}
	if (sc_find(plat, g->id) >= 0)
	{
		snprintf(msg, n, "%s is in the list already.", g->shown);
		return 0;
	}
	if (n_sc >= 64)
	{
		snprintf(msg, n, "Not added: the list holds 64 disc shortcuts at most.");
		return -2;
	}
	sc[n_sc].plat = plat;
	snprintf(sc[n_sc].id, sizeof sc[n_sc].id, "%s", g->id);
	snprintf(sc[n_sc].title, sizeof sc[n_sc].title, "%s", g->name);
	n_sc++;
	if (sc_save())
	{
		snprintf(msg, n, "Not added: could not write %s.", SHORTCUTS);
		return -3;
	}
	snprintf(msg, n, "Added %s. It stays in the list when the disc is out.", g->shown);
	return 0;
}

int shortcut_del(int plat, const game_t *g, char *msg, int n)
{
	int i;

	sc_load();
	i = sc_find(plat, g->id);
	if (i < 0)
	{
		snprintf(msg, n, "It was not in the list.");
		return -1;
	}
	memmove(&sc[i], &sc[i + 1], (n_sc - i - 1) * sizeof sc[0]);
	n_sc--;
	if (sc_save())
	{
		snprintf(msg, n, "Not removed: could not write %s.", SHORTCUTS);
		return -2;
	}
	snprintf(msg, n, "Removed %s from the list (the disc itself is untouched).", g->shown);
	return 0;
}

/* ---- the disc, added to the lists by games_scan() ---- */

static void add_game(int plat, const char *id, const char *title, int kind, int shortcut)
{
	game_t *g;

	if (n_games[plat] >= MAX_GAMES)
		return;
	g = &games[plat][n_games[plat]++];
	memset(g, 0, sizeof *g);
	snprintf(g->name, sizeof g->name, "%s", title);
	snprintf(g->path, sizeof g->path, "%s", BD);
	snprintf(g->id, sizeof g->id, "%s", id);
	g->kind = (u8)kind;
	g->where = WHERE_DISC;
	g->shortcut = (u8)shortcut;
}

/* v0.30: PS1 and PS2 discs. The console mounts no file system for them: no /dev_bdvd (seen 11-23
 * with a PS1 and a PS2 disc in the drive; Bananafuzzbuddy: "our app doesn't recognize it", while Borderlands
 * was), so the SYSTEM.CNF read below never ran. Cobra says what the physical disc is (syscall 8,
 * opcode 0x7020, webMAN-MOD cobra.h DiscType); the title ID comes from SYSTEM.CNF through the
 * drive's ISO 9660 file system, mounted just for the read the way webMAN's PS2-disc support does
 * (ps2_disc.h: "CELL_FS_IOS:BDVD_DRIVE", "CELL_FS_ISO9660", /dev_ps2disc, read-only) and unmounted
 * again. Without the ID the disc is still listed. One line per look: /dev_hdd0/tmp/prism_disc.log */
#include <ppu-lv2.h>

/* v0.32: the call answers with the system's own device types (webMAN-MOD cobra/storage.h; cobra.c
 * translate_type() turns them into its DISC_TYPE_* list, which v0.30-v0.31 wrongly used: a PS1
 * disc came back as 0xFF50 and was taken for "not PS1/PS2", prism_disc.log 11-23) */
#define DT_PS2_DVD	0xFF61
#define DT_PS2_CD	0xFF60
#define DT_PSX_CD	0xFF50
#define DT_DVD		0x10
#define DT_CD		0x08
#define DISC_MNT	"/dev_ps2disc"
#define DISC_LOG	"/dev_hdd0/tmp/prism_disc.log"

static s32 sc8_disc_type(u32 *real, u32 *eff, u32 *fake)
{
	lv2syscall4(8, 0x7020, (u64)(unsigned long)real, (u64)(unsigned long)eff, (u64)(unsigned long)fake);
	return_to_user_prog(s32);
}

static s32 fs_mount(const char *dev, const char *fs, const char *path)
{
	lv2syscall8(837, (u64)(unsigned long)dev, (u64)(unsigned long)fs, (u64)(unsigned long)path, 0, 1, 0, 0, 0);
	return_to_user_prog(s32);
}

static s32 fs_umount(const char *path)
{
	lv2syscall3(838, (u64)(unsigned long)path, 0, 1);
	return_to_user_prog(s32);
}

/* a SYSTEM.CNF's boot file; 1 = PS2 ("BOOT2 = cdrom0:\SLUS_209.46;1"), 0 = PS1
 * ("BOOT = cdrom:\SCUS_944.55;1"), -1 = none */
static int cnf_parse(char *d, char *id, int idn)
{
	char *p, *q;
	int k = 0, ps2;

	id[0] = 0;
	ps2 = strstr(d, "BOOT2") != NULL;
	p = strstr(d, ps2 ? "cdrom0:" : "cdrom:");
	if (!p)
		return -1;
	p += ps2 ? 7 : 6;
	while (*p == '\\' || *p == '/')
		p++;
	for (q = p; *q > ' ' && *q != ';' && k < idn - 1 && k < 11; q++)
		id[k++] = *q;
	id[k] = 0;
	return ps2;
}

static int cnf_id(const char *path, char *id, int idn)
{
	static char d[8192];
	FILE *f = fopen(path, "rb");
	int n;

	id[0] = 0;
	if (!f)
		return -1;
	n = (int)fread(d, 1, sizeof d - 1, f);
	fclose(f);
	d[n < 0 ? 0 : n] = 0;
	return cnf_parse(d, id, idn);
}

/* ---- v0.33: the drive read directly ----
 * A disc put in while Prism runs got no /dev_bdvd and no Cobra disc type (prism_disc.log 11-23: every
 * look said "real 0" while Bananafuzzbuddy put a PS1 and a PS2 disc in): with a game running the system
 * leaves a new disc alone. The drive itself still answers, through the lv2 storage calls Prism reads
 * the SD card with: its size (609), and 2048-byte sectors (602) - the ISO 9660 volume descriptor at
 * sector 16, the root directory, SYSTEM.CNF. */
#include <malloc.h>
#include "storage.h"

#define BDVD_DEV	0x101000000000006ULL

static u32 le32m(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static double uptime_now(void)
{
	u64 t;

	__asm__ volatile("mftb %0" : "=r"(t));
	return (double)t / 79800000.0;
}

static void disc_log(const char *fmt, ...)
{
	FILE *f = fopen(DISC_LOG, "a");
	va_list ap;

	if (!f)
		return;
	fprintf(f, "%8.1f s  ", uptime_now());
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

/* v0.40: one line into the disc log from outside this file (main.c: the system's disc events) */
void disc_event(const char *what, unsigned a, unsigned b)
{
	disc_log("%s (%x, %x)", what, a, b);
}

/* P_PS1, P_PS2 or -1; why: what was found */
static int drive_probe(char *id, int idn, char *why, int wn)
{
	static u8 *b;
	st_devinfo info;
	u32 fd = 0, got, root, rsz, s, i;
	s32 r;
	int plat = -1;

	id[0] = 0;
	memset(&info, 0, sizeof info);
	r = st_info(BDVD_DEV, &info);
	if (r || !info.total_sectors)
	{
		snprintf(why, wn, "drive: info %x, %llu sectors (no disc)", (u32)r, (unsigned long long)info.total_sectors);
		return -1;
	}
	if (!b)
		b = (u8 *)memalign(128, 2 * 2048);
	if (!b)
		return -1;
	r = st_open(BDVD_DEV, &fd);
	if (r)
	{
		snprintf(why, wn, "drive: %llu sectors of %u; open %x", (unsigned long long)info.total_sectors, info.sector_size, (u32)r);
		return -1;
	}
	if (info.sector_size != 2048 || st_read(fd, 16, 1, b, &got) != 0 || b[0] != 1 || memcmp(b + 1, "CD001", 5))
	{
		snprintf(why, wn, "drive: %llu sectors of %u; no ISO 9660 volume", (unsigned long long)info.total_sectors, info.sector_size);
		goto out;
	}
	root = le32m(b + 158);
	rsz = le32m(b + 166);
	for (s = 0; s < 4 && s * 2048 < rsz && plat < 0; s++)
	{
		if (st_read(fd, root + s, 1, b, &got) != 0)
			break;
		for (i = 0; i + 44 < 2048 && b[i]; i += b[i])
			if (b[i + 32] >= 10 && !memcmp(b + i + 33, "SYSTEM.CNF", 10))
			{
				u32 lba = le32m(b + i + 2);

				if (st_read(fd, lba, 1, b, &got) == 0)
				{
					b[2047] = 0;
					plat = cnf_parse((char *)b, id, idn);
					plat = plat == 1 ? P_PS2 : plat == 0 ? P_PS1 : -1;
				}
				break;
			}
	}
	snprintf(why, wn, "drive: %llu sectors of %u; SYSTEM.CNF %s", (unsigned long long)info.total_sectors, info.sector_size,
		plat >= 0 ? id : "not found");
out:
	st_close(fd);
	return plat;
}

int disc_ps12(char *id, int idn, char *title, int tn)
{
	u32 real = 0, eff = 0, fake = 0, t;
	s32 sr = sc8_disc_type(&real, &eff, &fake), mr = 1;
	int plat = -1, here = 0;
	char why[160];

	why[0] = 0;
	t = real;
	if ((t == DT_DVD || t == DT_CD) && (eff == DT_PS2_DVD || eff == DT_PS2_CD || eff == DT_PSX_CD))
		t = eff;	/* a burned disc the console takes for a PS1 / PS2 one */
	if (sr == 0)
		plat = (t == DT_PS2_DVD || t == DT_PS2_CD) ? P_PS2 : t == DT_PSX_CD ? P_PS1 : -1;
	if (plat >= 0)
	{
		if (!exists(DISC_MNT))
		{
			mr = fs_mount("CELL_FS_IOS:BDVD_DRIVE", "CELL_FS_ISO9660", DISC_MNT);
			here = mr == 0;
		}
		cnf_id(DISC_MNT "/SYSTEM.CNF", id, idn);
		if (here)
			fs_umount(DISC_MNT);
	}
	/* v0.35: no sector reads of the drive here. v0.33's drive_probe never read a PS2 disc (sector 16
	 * failed on every look, prism_disc.log 11-23), and after Prism had the drive open Cobra stopped
	 * reporting the disc at all; a disc put in while Prism runs is reported by Cobra after all
	 * (ff61 at 262 s, same log). */
	(void)drive_probe;
	if (plat >= 0)
		snprintf(title, tn, id[0] ? "%s disc %s" : "%s disc", plat == P_PS2 ? "PS2" : "PS1", id);
	disc_log("look: Cobra call %d, real %x, effective %x; ISO 9660 mount %d; %s -> %s %s", (int)sr, (unsigned)real, (unsigned)eff,
		(int)mr, why[0] ? why : "drive not read", plat == P_PS2 ? "PS2" : plat == P_PS1 ? "PS1" : "none", id);
	return plat;
}

/* v0.38: what kind of PS1 / PS2 disc Cobra says is in the drive (a burned disc the console takes for
 * one counts as that kind): DT_PSX_CD, DT_PS2_CD, DT_PS2_DVD, or 0 */
unsigned disc_ps_type(void)
{
	u32 real = 0, eff = 0, fake = 0, t;

	if (sc8_disc_type(&real, &eff, &fake) != 0)
		return 0;
	t = real;
	if ((t == DT_DVD || t == DT_CD) && (eff == DT_PS2_DVD || eff == DT_PS2_CD || eff == DT_PSX_CD))
		t = eff;
	return (t == DT_PS2_DVD || t == DT_PS2_CD || t == DT_PSX_CD) ? t : 0;
}

/* v0.44: 1 = Cobra is showing a game image in place of the drive (the last PS1 / PS3 game started
 * through webMAN): every read of "the drive" then returns the image, and the drive's size is the
 * image's. webMAN's own "mounted" answer can be "no" while this is so (Bananafuzzbuddy, 11-23: a Guitar Hero II
 * disc "copied" as Loaded, the PS1 game played just before - prism_disc.log 8309 s, "real ff61,
 * effective ff50"). A plain burned disc Cobra takes for a PS1 / PS2 one is not that (disc_ps_type). */
int disc_emulated(unsigned *real_out, unsigned *eff_out)
{
	u32 real = 0, eff = 0, fake = 0;

	if (sc8_disc_type(&real, &eff, &fake) != 0)
		real = eff = 0;
	if (real_out)
		*real_out = real;
	if (eff_out)
		*eff_out = eff;
	return eff != real && real != DT_DVD && real != DT_CD;
}

/* v0.31: a number that changes when a disc goes in or comes out: Cobra's type for the physical disc,
 * whether /dev_bdvd is there (a PS3 or PS1 disc's file system appears a few seconds after the type
 * does), and (v0.33) the drive's own sector count, which changes even when the system leaves the disc
 * alone. Changes go to the disc log. */
int disc_signature(void)
{
	static int last = -12345;
	u32 real = 0, eff = 0, fake = 0;
	st_devinfo info;
	int sig;

	if (sc8_disc_type(&real, &eff, &fake) != 0)
		real = 0xFF;
	memset(&info, 0, sizeof info);
	if (st_info(BDVD_DEV, &info) != 0)
		info.total_sectors = 0;
	sig = (int)((real & 0xFFFF) * 2 + (exists(BD) ? 1 : 0)) ^ (int)((info.total_sectors & 0x3FFFFF) << 9);
	if (sig != last)
		disc_log("drive watch: Cobra real %x, /dev_bdvd %s, drive %llu sectors", (unsigned)real, exists(BD) ? "yes" : "no",
			(unsigned long long)info.total_sectors);
	last = sig;
	return sig;
}

/* the disc in the drive: platform, title ID, title; -1 = none (or a mounted game hides it) */
static int disc_now(char *id, int idn, char *title, int tn)
{
	static u8 d[8192];
	FILE *f;
	int n;

	id[0] = title[0] = 0;
	if (wm_mounted(NULL, 0))
		return -1;
	if (!exists(BD))
		return disc_ps12(id, idn, title, tn);
	f = fopen(BD "/PS3_GAME/PARAM.SFO", "rb");
	if (f)
	{
		n = (int)fread(d, 1, sizeof d, f);
		fclose(f);
		sfo_get(d, n, "TITLE_ID", id, idn);
		if (sfo_get(d, n, "TITLE", title, tn) || !title[0])
			snprintf(title, tn, "%s", id);
		return id[0] ? P_PS3 : disc_ps12(id, idn, title, tn);
	}
	f = fopen(BD "/SYSTEM.CNF", "rb");
	if (f)
	{
		char *p, *q;
		int k = 0, ps2;

		n = (int)fread(d, 1, sizeof d - 1, f);
		fclose(f);
		d[n < 0 ? 0 : n] = 0;
		ps2 = strstr((char *)d, "BOOT2") != NULL;
		p = strstr((char *)d, ps2 ? "cdrom0:" : "cdrom:");
		if (!p)
			return -1;
		p += ps2 ? 7 : 6;
		while (*p == '\\' || *p == '/')
			p++;
		for (q = p; *q > ' ' && *q != ';' && k < idn - 1 && k < 11; q++)
			id[k++] = *q;
		id[k] = 0;
		snprintf(title, tn, "%s disc %s", ps2 ? "PS2" : "PS1", id);
		return ps2 ? P_PS2 : P_PS1;
	}
	return disc_ps12(id, idn, title, tn);
}

void scan_disc_and_shortcuts(void)
{
	char id[12], title[100];
	int p = disc_now(id, sizeof id, title, sizeof title), i;

	sc_load();
	if (p >= 0)
		add_game(p, id, title, 4, sc_find(p, id) >= 0);
	for (i = 0; i < n_sc; i++)
		if (!(p == sc[i].plat && !strcmp(id, sc[i].id)))
			add_game(sc[i].plat, sc[i].id, sc[i].title, 5, 1);
}

/* the files of a PS1 game or a PS3 .iso game (what delete would remove), for copy_game */
int game_files(int plat, const game_t *g, char (*out)[256], int max)
{
	char why[160];
	int i;

	if (plan(plat, g, why, sizeof why) || (plat == P_PS3 && g->kind == 3))
		return 0;	/* PS1, PSP and PS3 .iso games: their files */
	for (i = 0; i < n_del && i < max; i++)
		snprintf(out[i], 256, "%s", del_list[i]);
	return i;
}
