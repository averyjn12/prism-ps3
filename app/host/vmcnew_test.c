/* vmcnew_test: the app's new-card maker and card renamer (src/vmcnew.c, unchanged) on the PC.
 * Judged by host/vmcnew_test.sh: against the PS3's own empty card (by sha256), against host/ps1_card.py's
 * blank PS1 card, by the app's own reader, and by mymcplus when it is there.
 *
 *   vmcnew_test ps2 <out file> <16 hex digits: the stamp>     the image, as vmc_new_ps2_image makes it
 *   vmcnew_test ps1 <out file>
 *   vmcnew_test stamp <unix seconds>                           prints the 8 stamp bytes in hex
 *   vmcnew_test names                                          the name rules, a table; exit = failures
 *   vmcnew_test new <dir> <name> <0|1 ps1> <unix seconds>      vmc_new; prints "r <code> <why>"
 *   vmcnew_test rename <dir> <old file> <new name>             vmc_rename; prints "r <code> <new file> <why>"
 *   vmcnew_test load <card>                                    the app's reader: "ok N ps1 N saves N free N"
 *   vmcnew_test import <backup dir> <card> <file.psu>          vmc_import_psu into the card
 *   vmcnew_test export <card> <save folder> <out.psu> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vmc.h"

static int names(void)
{
	static const struct { const char *n; int ok; } t[] = {
		{ "Prism PS2 Card 1", 1 }, { "A", 1 }, { "a-b_c (2)", 1 }, { "0123456789012345678901234567", 1 } /* 28 */,
		{ "", 0 }, { "01234567890123456789012345678", 0 } /* 29 */, { " lead", 0 }, { "trail ", 0 }, { " ", 0 },
		{ "a/b", 0 }, { "a\\b", 0 }, { "a.b", 0 }, { "..", 0 }, { "a:b", 0 }, { "a*b", 0 }, { "a?b", 0 }, { "caf\xC3\xA9", 0 },
		{ "tab\there", 0 }, { "new\nline", 0 }, { "a\"b", 0 }, { "a<b", 0 }, { "a|b", 0 }, { "a'b", 0 }, { "a,b", 0 }, { "a+b", 0 },
		{ "a@b", 0 }, { "a[b", 0 }, { "a`b", 0 }, { "a{b", 0 }, { "a\x7f" "b", 0 }, { "Z9 z0", 1 }, { "()", 1 }, { "_", 1 }, { "-", 1 },
	};
	char why[120];
	unsigned i;
	int fail = 0;

	for (i = 0; i < sizeof t / sizeof t[0]; i++)
	{
		int got = vmc_name_ok(t[i].n, why, sizeof why) == 0;

		if (got != t[i].ok)
		{
			printf("FAIL name %u: want %d got %d\n", i, t[i].ok, got);
			fail++;
		}
	}
	/* the same-name rule: any case, either kind, nothing else */
	{
		static const struct { const char *f, *n; int same; } s[] = {
			{ "OPL.VM2", "OPL", 1 }, { "opl.vm2", "OPL", 1 }, { "OPL.VM1", "opl", 1 }, { "OPL.VM2", "OP", 0 }, { "OPL.VM2", "OPLX", 0 },
			{ "OPL.VM3", "OPL", 0 }, { "OPL.VM2.new", "OPL", 0 }, { "OPL", "OPL", 0 }, { "OPL2.VM2", "OPL", 0 }, { "XOPL.VM2", "OPL", 0 },
		};

		for (i = 0; i < sizeof s / sizeof s[0]; i++)
			if (vmc_same_name(s[i].f, s[i].n) != s[i].same)
			{
				printf("FAIL same %u: %s / %s want %d\n", i, s[i].f, s[i].n, s[i].same);
				fail++;
			}
	}
	printf("names: %d failure(s)\n", fail);
	return fail;
}

static int write_file(const char *p, const u8 *d, long n)
{
	FILE *f = fopen(p, "wb");

	if (!f || fwrite(d, 1, n, f) != (size_t)n)
		return 3;
	return fclose(f) ? 3 : 0;
}

int main(int argc, char **argv)
{
	char why[200] = "", nf[200] = "";
	int r;

	if (argc >= 4 && !strcmp(argv[1], "ps2"))
	{
		u8 *img = malloc(VMC_PS2_BYTES), st[8];
		int i;

		if (strlen(argv[3]) != 16)
			return 2;
		for (i = 0; i < 8; i++)
		{
			unsigned v;

			sscanf(argv[3] + 2 * i, "%2x", &v);
			st[i] = (u8)v;
		}
		vmc_new_ps2_image(img, st);
		return write_file(argv[2], img, VMC_PS2_BYTES);
	}
	if (argc >= 3 && !strcmp(argv[1], "ps1"))
	{
		u8 *img = malloc(VMC_PS1_BYTES);

		vmc_new_ps1_image(img);
		return write_file(argv[2], img, VMC_PS1_BYTES);
	}
	if (argc >= 3 && !strcmp(argv[1], "stamp"))
	{
		u8 st[8];
		int i;

		vmc_stamp(strtoull(argv[2], NULL, 10), st);
		for (i = 0; i < 8; i++)
			printf("%02x", st[i]);
		printf("\n");
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "names"))
		return names();
	if (argc >= 6 && !strcmp(argv[1], "new"))
	{
		r = vmc_new(argv[2], argv[3], atoi(argv[4]), strtoull(argv[5], NULL, 10), why, sizeof why);
		printf("r %d %s\n", r, why);
		return 0;
	}
	if (argc >= 5 && !strcmp(argv[1], "rename"))
	{
		r = vmc_rename(argv[2], argv[3], argv[4], nf, sizeof nf, why, sizeof why);
		printf("r %d [%s] %s\n", r, r ? "" : nf, why);
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "load"))
	{
		static card_t c;
		int i;

		vmc_load(&c, argv[2]);
		printf("ok %d ps1 %d saves %d free %d\n", c.ok, c.ps1, c.n, c.free_kb);
		for (i = 0; i < c.n; i++)
			printf("  %s %u bytes %d files\n", c.saves[i].dir, c.saves[i].bytes, c.saves[i].nfiles);
		return 0;
	}
	if (argc >= 5 && !strcmp(argv[1], "import"))
	{
		static card_t c;

		vmc_bak_dir = argv[2];
		vmc_load(&c, argv[3]);
		r = vmc_import_psu(argv[4], &c, why, sizeof why);
		printf("r %d %s\n", r, why);
		return 0;
	}
	if (argc >= 5 && !strcmp(argv[1], "export"))
	{
		static card_t c;
		int i, si = -1;

		vmc_load(&c, argv[2]);
		for (i = 0; i < c.n; i++)
			if (!strcmp(c.saves[i].dir, argv[3]))
				si = i;
		r = si < 0 ? -99 : vmc_export_psu(&c, si, argv[4], why, sizeof why);
		printf("r %d %s\n", r, why);
		return 0;
	}
	return 2;
}
