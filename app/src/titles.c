/* v0.43: game titles by title ID, for discs and images Prism has no cover-and-title entry for
 * (art.c knows only the games bench/lr_art.py packed). Bananafuzzbuddy, 11-23, after the first PS1 disc copy:
 * "it's called slus though for some reason" - the copy was named SLUS_000.76 because nothing on the
 * console knew that ID is "Loaded".
 * USRDIR/titles.txt is made at build time (tools/mk_titles.py) from the two lists in db/: one line
 * per game, "<ID>|<title>", sorted by ID, plain ASCII. Loaded whole on first use. Plain C. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "titles.h"

const char *titles_path = "/dev_hdd0/game/PRSM00001/USRDIR/titles.txt";
static char *tx;
static long tx_n;
static int tried;

static void load(void)
{
	FILE *f;
	long n;

	tried = 1;
	f = fopen(titles_path, "rb");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n > 0 && n < (8L << 20))
	{
		tx = (char *)malloc((size_t)n + 1);
		if (tx && (long)fread(tx, 1, (size_t)n, f) == n)
		{
			tx[n] = 0;
			tx_n = n;
		}
		else
		{
			free(tx);
			tx = NULL;
		}
	}
	fclose(f);
}

/* the start of the line that holds byte p */
static long line_at(long p)
{
	while (p > 0 && tx[p - 1] != '\n')
		p--;
	return p;
}

int title_lookup(const char *id, char *out, int n)
{
	long lo = 0, hi, idn = (long)strlen(id);

	if (n > 0)
		out[0] = 0;
	if (!tried)
		load();
	if (!tx || idn < 4 || idn > 16)
		return -1;
	hi = tx_n;
	while (lo < hi)
	{
		long mid = line_at(lo + (hi - lo) / 2), end = mid, k;
		int c;

		while (end < tx_n && tx[end] != '\n')
			end++;
		for (k = 0; mid + k < end && tx[mid + k] != '|'; k++)
			;
		c = strncmp(id, tx + mid, (size_t)(k < idn ? k : idn));
		if (!c)
			c = (int)(idn - k);
		if (!c)
		{
			long t = mid + k + 1, m = 0;

			while (t + m < end && tx[t + m] != '\r' && m < n - 1)
			{
				out[m] = tx[t + m];
				m++;
			}
			out[m] = 0;
			return m ? 0 : -1;
		}
		if (c < 0)
			hi = mid;
		else
			lo = end + 1;
	}
	return -1;
}
