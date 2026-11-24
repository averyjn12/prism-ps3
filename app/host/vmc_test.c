/* vmc_test: the app's card writer (src/vmc.c + src/vmcw.c, unchanged) run on the PC against
 * COPIES of real cards. Judged outside this program by host/vmc_test.sh (mymcplus, an
 * independent PS2 card reader, plus vmc_inspect.py's ECC count).
 *
 *   vmc_test <backup dir> copy <from card> <save folder> <to card> [expect-fail]
 *   vmc_test <backup dir> delete <card> <save folder> [expect-fail]
 *   vmc_test <backup dir> list <card>
 * Exit 0 when the result is the expected one. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vmc.h"

static int find(card_t *c, const char *dir)
{
	int i;

	for (i = 0; i < c->n; i++)
		if (!strcmp(c->saves[i].dir, dir))
			return i;
	return -1;
}

static void list(card_t *c)
{
	int i;

	printf("   %s: ok %d ps1 %d, %d save(s), %d KB free\n", c->name, c->ok, c->ps1, c->n, c->free_kb);
	for (i = 0; i < c->n; i++)
		printf("      %-24s %-36s %7u B %2d file(s) icon %d\n", c->saves[i].dir, c->saves[i].title, c->saves[i].bytes, c->saves[i].nfiles, c->saves[i].icon);
}

int main(int argc, char **argv)
{
	static card_t a, b;
	char why[200];
	int r = -1, si, expect_fail;

	if (argc < 4)
		return 2;
	vmc_bak_dir = argv[1];
	if (!strcmp(argv[2], "list"))
	{
		if (vmc_load(&a, argv[3]))
			return 1;
		list(&a);
		return 0;
	}
	if (!strcmp(argv[2], "copy") && argc >= 6)
	{
		expect_fail = argc > 6;
		if (vmc_load(&a, argv[3]) || vmc_load(&b, argv[5]))
		{
			printf("cannot load\n");
			return 1;
		}
		si = find(&a, argv[4]);
		if (si < 0)
		{
			printf("no save %s on %s\n", argv[4], a.name);
			return 1;
		}
		r = vmc_copy(&a, si, &b, why, sizeof why);
		printf("copy %s %s -> %s: %s%s\n", a.name, argv[4], b.name, r ? "REFUSED: " : "done", r ? why : "");
		vmc_free(&b);
		vmc_load(&b, argv[5]);
		list(&b);
		if (!r && find(&b, argv[4]) < 0)
		{
			printf("FAIL: the copy is not on the card when read back\n");
			return 1;
		}
		if (!r && b.saves[find(&b, argv[4])].bytes != a.saves[si].bytes)
		{
			printf("FAIL: size differs when read back\n");
			return 1;
		}
	}
	else if (!strcmp(argv[2], "delete") && argc >= 5)
	{
		expect_fail = argc > 5;
		if (vmc_load(&a, argv[3]))
			return 1;
		si = find(&a, argv[4]);
		if (si < 0)
		{
			printf("no save %s on %s\n", argv[4], a.name);
			return 1;
		}
		r = vmc_delete(&a, si, why, sizeof why);
		printf("delete %s %s: %s%s\n", a.name, argv[4], r ? "REFUSED: " : "done", r ? why : "");
		vmc_free(&a);
		vmc_load(&a, argv[3]);
		list(&a);
		if (!r && find(&a, argv[4]) >= 0)
		{
			printf("FAIL: still there when read back\n");
			return 1;
		}
	}
	else
		return 2;
	if (expect_fail ? r == 0 : r != 0)
	{
		printf("FAIL: expected %s\n", expect_fail ? "a refusal" : "success");
		return 1;
	}
	return 0;
}
