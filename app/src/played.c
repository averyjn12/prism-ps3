/* v0.67: the played list (played.h). The file: a first line "PRISMPLAYED1", then one line a game,
 *   <platform> TAB <times> TAB <order> TAB <clock> TAB <file name>
 * most recent first. A line that does not parse is skipped, so a damaged file loses that game's
 * history and nothing else. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "played.h"

#define HEAD	"PRISMPLAYED1"

static char path[200] = "/dev_hdd0/game/PRSM00001/USRDIR/played.txt";
static played_t list[PLAYED_MAX], before[PLAYED_MAX];	/* list: most recent first */
static int n_list, n_before = -1;

void played_file(const char *p)
{
	snprintf(path, sizeof path, "%s", p);
}

static int by_seq(const void *a, const void *b)
{
	const played_t *x = a, *y = b;

	return x->seq < y->seq ? 1 : x->seq > y->seq ? -1 : 0;
}

int played_load(void)
{
	FILE *f = fopen(path, "rb");
	char ln[400];

	n_list = 0;
	n_before = -1;
	if (!f)
		return 0;
	if (fgets(ln, sizeof ln, f) && !strncmp(ln, HEAD, strlen(HEAD)))
		while (n_list < PLAYED_MAX && fgets(ln, sizeof ln, f))
		{
			played_t *e = &list[n_list];
			unsigned p, c;
			unsigned long long s, t;
			int at = 0;
			size_t len;

			if (sscanf(ln, "%u\t%u\t%llu\t%llu\t%n", &p, &c, &s, &t, &at) != 4 || !at || p > 255 || !c)
				continue;
			len = strcspn(ln + at, "\r\n");
			if (!len || len >= PLAYED_NAME)
				continue;
			memset(e, 0, sizeof *e);
			e->plat = (unsigned char)p;
			e->count = c;
			e->seq = s;
			e->last = t;
			memcpy(e->name, ln + at, len);
			if (played_find((int)p, e->name))
				continue;	/* a game twice: the first line (the more recent) stands */
			n_list++;
		}
	fclose(f);
	qsort(list, n_list, sizeof list[0], by_seq);
	return n_list;
}

static int save(void)
{
	FILE *f = fopen(path, "wb");
	int i, bad = 0;

	if (!f)
		return -1;
	if (fprintf(f, HEAD "\n") < 0)
		bad = 1;
	for (i = 0; i < n_list && !bad; i++)
		if (fprintf(f, "%u\t%u\t%llu\t%llu\t%s\n", list[i].plat, list[i].count, list[i].seq, list[i].last, list[i].name) < 0)
			bad = 1;
	if (fclose(f) != 0)
		bad = 1;
	return bad ? -1 : 0;
}

const played_t *played_find(int plat, const char *name)
{
	int i;

	for (i = 0; i < n_list; i++)
		if (list[i].plat == plat && !strcmp(list[i].name, name))
			return &list[i];
	return NULL;
}

int played_count(void)
{
	return n_list;
}

const played_t *played_recent(int i)
{
	return i >= 0 && i < n_list ? &list[i] : NULL;
}

int played_mark(int plat, const char *name, unsigned long long now)
{
	played_t e;
	int i, at = -1;

	if (!name || !name[0] || strlen(name) >= PLAYED_NAME || strpbrk(name, "\t\r\n") || plat < 0 || plat > 255)
		return -2;
	memcpy(before, list, sizeof list);
	n_before = n_list;
	for (i = 0; i < n_list; i++)
		if (list[i].plat == plat && !strcmp(list[i].name, name))
			at = i;
	memset(&e, 0, sizeof e);
	if (at >= 0)
		e = list[at];
	else
	{
		e.plat = (unsigned char)plat;
		strcpy(e.name, name);
		if (n_list < PLAYED_MAX)
			at = n_list++;
		else
			at = PLAYED_MAX - 1;	/* full: the game started longest ago makes room */
	}
	e.count++;
	e.seq = n_before > 0 ? list[0].seq + 1 : 1;	/* list[0] is the most recent start so far */
	e.last = now;
	memmove(list + 1, list, (size_t)at * sizeof list[0]);
	list[0] = e;
	return save();
}

void played_undo(void)
{
	if (n_before < 0)
		return;
	memcpy(list, before, sizeof list);
	n_list = n_before;
	n_before = -1;
	save();
}

void played_ago(char *out, unsigned n, unsigned long long last, unsigned long long now)
{
	unsigned long long d;

	out[0] = 0;
	if (!last || !now || last > now || now - last > 20ull * 366 * 86400)
		return;
	d = now - last;
	if (d < 90)
		snprintf(out, n, "just now");
	else if (d < 3600)
		snprintf(out, n, "%u minutes ago", (unsigned)(d / 60));
	else if (d < 2 * 3600)
		snprintf(out, n, "an hour ago");
	else if (d < 86400)
		snprintf(out, n, "%u hours ago", (unsigned)(d / 3600));
	else if (d < 2 * 86400)
		snprintf(out, n, "yesterday");
	else if (d < 60ull * 86400)
		snprintf(out, n, "%u days ago", (unsigned)(d / 86400));
	else if (d < 730ull * 86400)
		snprintf(out, n, "%u months ago", (unsigned)(d / (30 * 86400)));
	else
		snprintf(out, n, "%u years ago", (unsigned)(d / (365 * 86400)));
}
