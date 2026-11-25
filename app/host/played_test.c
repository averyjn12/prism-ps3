/* v0.67: src/played.c on the PC.   bash host/played_test.sh   -> "played_test: N checks, 0 failed" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "played.h"

static int checks, failed;

static void check(int ok, const char *what)
{
	checks++;
	if (!ok)
	{
		failed++;
		printf("FAIL: %s\n", what);
	}
}

static const played_t none;
#define R(i)	(played_recent(i) ? played_recent(i) : &none)
#define F(p, n)	(played_find(p, n) ? played_find(p, n) : &none)

static void ago(unsigned long long d, const char *want)
{
	char t[40], w[100];

	played_ago(t, sizeof t, 1000000, 1000000 + d);
	snprintf(w, sizeof w, "%llu s ago reads \"%s\", expected \"%s\"", d, t, want);
	check(!strcmp(t, want), w);
}

int main(int argc, char **argv)
{
	const char *file = argc > 1 ? argv[1] : "/tmp/played_test.txt";
	char nm[64], t[40];
	FILE *f;
	int i;

	remove(file);
	played_file(file);
	check(played_load() == 0, "no file: an empty list");
	check(played_recent(0) == NULL && played_find(1, "A.iso") == NULL, "empty list: nothing found");
	check(played_mark(1, "A.iso", 5000) == 0, "first mark saves");
	check(played_mark(1, "B.iso", 6000) == 0, "second mark saves");
	check(played_mark(3, "A.iso", 7000) == 0, "the same name on another platform is another game");
	check(played_mark(1, "A.iso", 8000) == 0, "third mark saves");
	check(played_count() == 3, "three games");
	check(played_recent(0) && !strcmp(R(0)->name, "A.iso") && R(0)->plat == 1 && R(0)->count == 2
		&& R(0)->last == 8000, "the game started last is first, with its count and clock");
	check(played_recent(1) && R(1)->plat == 3 && played_recent(2) && !strcmp(R(2)->name, "B.iso"), "the order of the others");
	check(R(0)->seq > R(1)->seq && R(1)->seq > R(2)->seq, "the order numbers fall");
	/* read back from the file */
	check(played_load() == 3, "reloaded: three games");
	check(played_recent(0) && !strcmp(R(0)->name, "A.iso") && R(0)->count == 2 && R(0)->last == 8000, "reloaded: the first");
	check(played_find(3, "A.iso") && F(3, "A.iso")->count == 1 && !played_find(2, "A.iso"), "reloaded: found by platform and name");
	/* a start that failed is taken back, in memory and in the file */
	played_mark(1, "B.iso", 9000);
	check(!strcmp(R(0)->name, "B.iso") && R(0)->count == 2, "marked B");
	played_undo();
	check(!strcmp(R(0)->name, "A.iso") && F(1, "B.iso")->count == 1, "undo: as before, in memory");
	check(played_load() == 3 && !strcmp(R(0)->name, "A.iso") && F(1, "B.iso")->count == 1, "undo: as before, in the file");
	played_undo();
	check(played_count() == 3, "a second undo changes nothing");
	/* a first-ever start taken back leaves an empty list */
	remove(file);
	played_load();
	played_mark(0, "Only.bin", 100);
	played_undo();
	check(played_count() == 0 && played_load() == 0, "undo of the only game: empty");
	/* names no line could hold are refused, nothing is written */
	check(played_mark(1, "bad\tname", 1) != 0 && played_mark(1, "", 1) != 0 && played_mark(1, "two\nlines", 1) != 0 && played_count() == 0, "names with a tab or a line end are refused");
	/* names with spaces, brackets, a middle dot byte, UTF-8 */
	check(played_mark(2, "Borderlands\xc2\xae (Disc 1) [v1.2].iso", 50) == 0 && played_load() == 1
		&& !strcmp(R(0)->name, "Borderlands\xc2\xae (Disc 1) [v1.2].iso"), "a name with spaces, brackets and UTF-8 survives the file");
	/* more games than the list holds: the ones started longest ago go, the newest stay */
	remove(file);
	played_load();
	for (i = 0; i < PLAYED_MAX + 25; i++)
	{
		snprintf(nm, sizeof nm, "game %03d.iso", i);
		played_mark(1, nm, 1000 + i);
	}
	check(played_count() == PLAYED_MAX, "the list is full, not past it");
	snprintf(nm, sizeof nm, "game %03d.iso", PLAYED_MAX + 24);
	check(!strcmp(R(0)->name, nm), "the newest is first");
	check(played_find(1, "game 000.iso") == NULL && played_find(1, "game 024.iso") == NULL && played_find(1, "game 025.iso") != NULL, "the 25 oldest went");
	check(played_load() == PLAYED_MAX && !strcmp(R(0)->name, nm), "full list reloaded");
	/* a damaged file: bad lines are skipped, good ones kept, a second line for a game ignored */
	f = fopen(file, "wb");
	fprintf(f, "PRISMPLAYED1\n1\t4\t9\t777\tGood.iso\nrubbish line\n1\t0\t3\t5\tZeroCount.iso\n\t\t\t\t\n2\t1\t8\t0\tOther.iso\n1\t9\t2\t1\tGood.iso\n1\t1\t7\t1\t\n");
	fclose(f);
	check(played_load() == 2, "damaged file: two good games");
	check(!strcmp(R(0)->name, "Good.iso") && R(0)->count == 4 && !strcmp(R(1)->name, "Other.iso"), "damaged file: the right two, in order");
	f = fopen(file, "wb");
	fprintf(f, "SOMETHING ELSE\n1\t4\t9\t777\tGood.iso\n");
	fclose(f);
	check(played_load() == 0, "a file without the heading is not read");
	/* the words */
	ago(10, "just now");
	ago(600, "10 minutes ago");
	ago(3600, "an hour ago");
	ago(5 * 3600, "5 hours ago");
	ago(30 * 3600, "yesterday");
	ago(3 * 86400 + 5, "3 days ago");
	ago(90ull * 86400, "3 months ago");
	ago(800ull * 86400, "2 years ago");
	played_ago(t, sizeof t, 0, 5000);
	check(!t[0], "no clock at the start: no words");
	played_ago(t, sizeof t, 9000, 5000);
	check(!t[0], "a start in the future (the clock was reset): no words");
	printf("played_test: %d checks, %d failed\n", checks, failed);
	return failed != 0;
}
