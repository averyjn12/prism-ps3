/* src/covers_core.c on the PC, driven by host/covers_test.py (which holds the judges: PIL for the
 * scaling, bench/lr_art.py for the art file, and its own reader of the format).
 *   t unit <cover_sources.txt>            the serial, template and source-list checks (PASS / FAIL lines)
 *   t scale <in.argb> <w> <h> <out.rgba>  a picture (A R G B bytes) to one 128 x 128 cover
 *   t merge <old.dat|-> <out.dat> [ID plat title cover.rgba|-] ...
 *                                         the old file's records, then each given one added or, for an ID
 *                                         already there, its cover replaced (what covers.c rebuild does)
 *   t parse <file.dat>                    one line a record: ID, plat, has-cover, title */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "covers_core.h"

static int fails, passes;

static void check(int ok, const char *what)
{
	printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
	if (ok)
		passes++;
	else
		fails++;
}

static unsigned char *slurp(const char *p, long *len)
{
	FILE *f = fopen(p, "rb");
	unsigned char *b;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc(*len + 1);
	if (b && fread(b, 1, *len, f) != (size_t)*len)
	{
		free(b);
		b = NULL;
	}
	if (b)
		b[*len] = 0;
	fclose(f);
	return b;
}

static int unit(const char *srcfile)
{
	char o[400], d[12];
	cv_src s[CV_SRC_MAX];
	long len;
	char *text;
	int n;

	check(cv_is_id("SLUS_212.43"), "SLUS_212.43 is a serial");
	check(cv_is_id("SCES_500.51"), "SCES_500.51 is a serial");
	check(!cv_is_id("SLUS_21243"), "SLUS_21243 is not");
	check(!cv_is_id("SLUS-212.43"), "SLUS-212.43 is not");
	check(!cv_is_id("SLUS_212.4"), "a short one is not");
	check(!cv_is_id("SLUS_212.433"), "a long one is not");
	check(!cv_is_id("SL1S_212.43"), "a digit in the prefix is not");
	check(!cv_is_id("SLUS_2X2.43"), "a letter in the number is not");
	check(!cv_is_id(""), "empty is not");
	check(cv_is_id("BLUS30386"), "BLUS30386 (PS3) is a serial");
	check(cv_is_id("ULUS10041"), "ULUS10041 (PSP) is a serial");
	check(!cv_is_id("BLUS3038"), "a short PS3 one is not");
	check(!cv_is_id("BLU530386"), "a digit in a PS3 prefix is not");
	check(!cv_is_id("BLUS3O386"), "a letter in a PS3 number is not");
	check(!cv_is_id("BLUS-30386"), "the dash form is not (the game never writes it)");
	cv_dash("SLUS_212.43", d);
	check(!strcmp(d, "SLUS-21243"), "SLUS_212.43 -> SLUS-21243");
	cv_dash("SCUS_971.13", d);
	check(!strcmp(d, "SCUS-97113"), "SCUS_971.13 -> SCUS-97113");
	cv_dash("BLUS30386", d);
	check(!strcmp(d, "BLUS-30386"), "BLUS30386 -> BLUS-30386");
	check(cv_expand("/a/{ID}_COV.JPG", "SLUS_212.43", o, sizeof o) == 0 && !strcmp(o, "/a/SLUS_212.43_COV.JPG"), "{ID} expands");
	check(cv_expand("x{DASH}.jpg{DASH}", "SLUS_212.43", o, sizeof o) == 0 && !strcmp(o, "xSLUS-21243.jpgSLUS-21243"), "{DASH} expands, twice");
	check(cv_expand("{ID}", "SLUS_212.43", o, 11) == -1, "a result that does not fit is refused");
	check(cv_expand("{ID}", "SLUS_212.43", o, 12) == 0, "a result that just fits is kept");
	check(cv_expand("/x/{ID}", "../../etc", o, sizeof o) == -1, "no serial, no path (nothing else reaches a URL)");
	check(cv_expand("{ID", "SLUS_212.43", o, sizeof o) == 0 && !strcmp(o, "{ID"), "a broken brace is copied as it is");

	n = cv_sources_parse("# c\n\nA|2|https://h.example/p/{DASH}.jpg\nB | 1 | /dev_x/{ID}.JPG\r\nC|12|http://plain/x\nD|5|https://h/x\n"
		"E|2|ftp://x\nF|2|https://nopath\n|2|https://h/x\nG|2\nH | 34 | game:ICON0.PNG\nI|4|game:\n", s, CV_SRC_MAX);
	check(n == 3, "sources: only the three good lines of eleven are taken");
	check(n >= 1 && !strcmp(s[0].name, "A") && s[0].plats == 4 && !s[0].local && !strcmp(s[0].host, "h.example") && !strcmp(s[0].path, "/p/{DASH}.jpg"),
		"sources: a web line (host, path, PS2)");
	check(n >= 2 && !strcmp(s[1].name, "B") && s[1].plats == 2 && s[1].local == 1 && !strcmp(s[1].path, "/dev_x/{ID}.JPG"), "sources: a local line, spaces and CR trimmed (PS1)");
	check(n >= 3 && !strcmp(s[2].name, "H") && s[2].plats == ((1 << 3) | (1 << 4)) && s[2].local == 2 && !strcmp(s[2].path, "ICON0.PNG"),
		"sources: a game's own picture (PS3 and PSP)");

	text = (char *)slurp(srcfile, &len);
	check(text != NULL, "the shipped cover_sources.txt reads");
	if (!text)
		return 1;
	n = cv_sources_parse(text, s, CV_SRC_MAX);
	check(n == 8, "the shipped list has 8 sources");
	check(n == 8 && s[0].local == 2 && s[0].plats == 0x1E && !strcmp(s[0].path, "BESIDE"), "0: a picture beside the game, every platform");
	if (n == 8)
	{
		char f[208];

		/* v0.59: which sources wait for a folder (0.58 asked it of game: sources, and never offered the PSP icon) */
		check(cv_src_folder(&s[0], f, sizeof f) == 0, "offered always: the picture beside the game");
		check(cv_src_folder(&s[7], f, sizeof f) == 0, "offered always: the game's own icon (0.58 hid it)");
		check(cv_src_folder(&s[3], f, sizeof f) == 0, "offered always: a web source");
		check(cv_src_folder(&s[2], f, sizeof f) == 1 && !strcmp(f, "/dev_hdd0/game/BLES80608/USRDIR/covers"), "a cover pack waits for its folder");
		memmove(s, s + 1, sizeof s[0] * 7);	/* the checks below were written for the list without line 0 */
	}
	if (n == 8)
	{
		char p[300];

		/* v0.59: the names a picture beside a game may have */
		check(cv_beside("/dev_hdd0/PSPISO/Call of Duty - Roads to Victory (USA).iso", 0, p, sizeof p) == 0
			&& !strcmp(p, "/dev_hdd0/PSPISO/Call of Duty - Roads to Victory (USA).png"), "beside: the .png webMAN left next to Bananafuzzbuddy's PSP game (FTP, 11-24)");
		check(cv_beside("/dev_usb002/PSPISO/x.iso", 1, p, sizeof p) == 0 && !strcmp(p, "/dev_usb002/PSPISO/x.jpg"), "beside: then .jpg");
		check(cv_beside("/dev_usb002/PSPISO/x.iso", 3, p, sizeof p) == 0 && !strcmp(p, "/dev_usb002/PSPISO/x.JPG"), "beside: and .JPG last");
		check(cv_beside("/dev_usb002/PSPISO/x.iso", 4, p, sizeof p) == -1, "beside: four names, no fifth");
		check(cv_beside("/PS2ISO/Game.iso", 0, p, sizeof p) == -1, "beside: not for a card game (its path is the card's, not the console's)");
		check(cv_beside("/dev_hdd0/GAMES/BLUS30386", 0, p, sizeof p) == -1, "beside: not for a game folder");
		check(cv_beside("/dev_hdd0/a.b/c", 0, p, sizeof p) == -1, "beside: a dot in a folder name is not an extension");
		check(cv_beside("/dev_hdd0/PSPISO/.iso", 0, p, sizeof p) == -1, "beside: a name that is only an extension");
		check(cv_beside("/dev_hdd0/PSPISO/abc.iso", 0, p, 24) == -1, "beside: a name one byte too long for the buffer is refused");
		check(cv_beside("/dev_hdd0/PSPISO/abc.iso", 0, p, 25) == 0 && !strcmp(p, "/dev_hdd0/PSPISO/abc.png"), "beside: a name that just fits is kept");
	}
	if (n == 8)
	{
		check(s[0].local == 1 && s[0].plats == 6, "1: the installed cover pack, PS1 and PS2");
		check(cv_expand(s[0].path, "SLUS_212.00", o, sizeof o) == 0 && !strcmp(o, "/dev_hdd0/game/BLES80608/USRDIR/covers_retro/psx/SLUS_212.00_COV.JPG"),
			"1: the pack's own name for SLUS_212.00 (pkg_list.py read it from the pkg)");
		check(s[1].local == 1 && s[1].plats == 8 && cv_expand(s[1].path, "BLUS30386", o, sizeof o) == 0
			&& !strcmp(o, "/dev_hdd0/game/BLES80608/USRDIR/covers/BLUS30386.JPG"), "2: the PS3 cover pack, where webMAN's wiki puts it");
		check(!s[2].local && s[2].plats == 4 && !strcmp(s[2].host, "raw.githubusercontent.com"), "3: PS2 xlenore, on GitHub");
		check(cv_expand(s[2].path, "SLUS_212.43", o, sizeof o) == 0 && !strcmp(o, "/xlenore/ps2-covers/main/covers/default/SLUS-21243.jpg"),
			"3: the path the mule fetched MGS3's cover from on 11-24 (Net Test)");
		check(s[3].plats == 2 && cv_expand(s[3].path, "SLUS_000.67", o, sizeof o) == 0 && !strcmp(o, "/xlenore/psx-covers/main/covers/default/SLUS-00067.jpg"),
			"4: PS1 xlenore, same form");
		check(s[4].plats == 4 && cv_expand(s[4].path, "SLUS_212.43", o, sizeof o) == 0 && !strcmp(o, "/CosmicScale/psbbn-art-database/main/art/SLUS_212.43.png"),
			"5: PSBBN, the path bench/lr_art.py uses");
		check(s[5].plats == 8 && !strcmp(s[5].host, "raw.githubusercontent.com") && cv_expand(s[5].path, "BLUS30386", o, sizeof o) == 0
			&& !strcmp(o, "/aldostools/resources/master/COV/BLUS30386.JPG"), "6: PS3 aldostools, the URL that answered 200 for Borderlands on 11-24");
		check(s[6].local == 2 && s[6].plats == ((1 << 3) | (1 << 4)) && !strcmp(s[6].path, "ICON0.PNG"), "7: the game's own icon, PS3 and PSP");
	}
	printf("---- %d passed, %d failed\n", passes, fails);
	return fails != 0;
}

static int scale(char **a, int how)
{
	long len;
	unsigned char *in = slurp(a[0], &len), out[CV_PIX];
	int w = atoi(a[1]), h = atoi(a[2]), r;
	FILE *f;

	if (!in || len != (long)w * h * 4)
		return 2;
	r = how == 2 ? cv_cover(in, w, h, w * 4, out) : how ? cv_fit(in, w, h, w * 4, out) : cv_scale(in, w, h, w * 4, out);
	if (r)
		return 2;
	f = fopen(a[3], "wb");
	fwrite(out, 1, CV_PIX, f);
	fclose(f);
	return 0;
}

static int merge(int argc, char **a)
{
	static cv_entry e[CV_REC_MAX + 600];
	long len = 0;
	unsigned char *old = strcmp(a[0], "-") ? slurp(a[0], &len) : NULL;
	int n = old ? cv_art_parse(old, len, e, CV_REC_MAX) : 0, i, k, r;
	FILE *f;

	if (n < 0)
		return 3;
	if (argc == 3 && a[2][0] == '@')
	{
		/* the entries from a file, one a line: ID TAB plat TAB title TAB cover (Windows' command line is short) */
		static char *v[4 * 1200 + 2];
		static char ln[1200][400];
		FILE *lf = fopen(a[2] + 1, "rb");
		int m = 2;

		if (!lf)
			return 6;
		v[0] = a[0];
		v[1] = a[1];
		for (i = 0; i < 1200 && fgets(ln[i], sizeof ln[i], lf); i++)
		{
			char *p = ln[i];
			int k2;

			p[strcspn(p, "\r\n")] = 0;
			for (k2 = 0; k2 < 4; k2++)
			{
				v[m++] = p;
				p = strchr(p, '\t');
				if (!p)
					break;
				*p++ = 0;
			}
			if (k2 < 3)
				return 7;
		}
		fclose(lf);
		argc = m;
		a = v;
	}
	for (i = 2; i + 3 < argc; i += 4)
	{
		long cl;
		unsigned char *px = strcmp(a[i + 3], "-") ? slurp(a[i + 3], &cl) : NULL;

		for (k = 0; k < n && strcmp(e[k].id, a[i]); k++)
			;
		if (k == n)
		{
			memset(&e[n], 0, sizeof e[n]);
			snprintf(e[n].id, sizeof e[n].id, "%s", a[i]);
			snprintf(e[n].title, sizeof e[n].title, "%s", a[i + 2]);
			n++;
		}
		/* plat 0..4, or "r": the player removed this one's cover (covers.c covers_remove) */
		if (a[i + 1][0] == 'r')
		{
			e[k].refused = 1;
			e[k].px = NULL;
			e[k].title[0] = 0;
		}
		else
			e[k].plat = (unsigned char)atoi(a[i + 1]);
		if (px)
			e[k].px = px;
	}
	f = fopen(a[1], "wb");
	r = cv_art_write(e, n, f);
	fclose(f);
	printf("%d covers\n", r);
	return r < 0 ? 4 : 0;
}

static int parse(const char *p)
{
	static cv_entry e[CV_REC_MAX];
	long len;
	unsigned char *b = slurp(p, &len);
	int n = b ? cv_art_parse(b, len, e, CV_REC_MAX) : -9, i;

	if (n < 0)
	{
		printf("not an art file (%d)\n", n);
		return 5;
	}
	for (i = 0; i < n; i++)
		printf("%s\t%d\t%d\t%d\t%s\n", e[i].id, e[i].plat, e[i].refused != 0, e[i].px != NULL, e[i].title);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "unit"))
		return unit(argv[2]);
	if (argc == 6 && !strcmp(argv[1], "scale"))
		return scale(argv + 2, 0);
	if (argc == 6 && !strcmp(argv[1], "fit"))
		return scale(argv + 2, 1);
	if (argc == 6 && !strcmp(argv[1], "cover"))
		return scale(argv + 2, 2);
	if (argc >= 4 && !strcmp(argv[1], "merge"))
		return merge(argc - 2, argv + 2);
	if (argc == 3 && !strcmp(argv[1], "parse"))
		return parse(argv[2]);
	fprintf(stderr, "usage: see the top of covers_test.c\n");
	return 9;
}
