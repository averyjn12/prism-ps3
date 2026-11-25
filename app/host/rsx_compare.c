/* v0.67: compare two sets of renders of the same scenes, picture by picture (the CPU drawing against the
 * RSX path's, see render.c).
 *   rsx_compare <dir A> <dir B> <diff dir> name [name ...]
 * For each name: A/name.ppm against B/name.ppm. Prints the largest channel difference, the mean, and how
 * many pixels differ by more than 8 and by more than 32 of 255 in a channel; writes <diff dir>/name.ppm
 * with the difference x 8 for a look. A picture FAILS when more than 0.5% of its pixels differ by more
 * than 32, or the sizes differ; the exit code is the number that failed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *load(const char *dir, const char *name, int *w, int *h)
{
	char p[600];
	FILE *f;
	unsigned char *d;
	int mx;

	snprintf(p, sizeof p, "%s/%s.ppm", dir, name);
	f = fopen(p, "rb");
	if (!f)
		return NULL;
	if (fscanf(f, "P6 %d %d %d", w, h, &mx) != 3)
	{
		fclose(f);
		return NULL;
	}
	fgetc(f);
	d = malloc((size_t)*w * *h * 3);
	if (fread(d, 3, (size_t)*w * *h, f) != (size_t)*w * *h)
	{
		free(d);
		d = NULL;
	}
	fclose(f);
	return d;
}

int main(int argc, char **argv)
{
	int k, failed = 0;

	if (argc < 5)
		return 99;
	for (k = 4; k < argc; k++)
	{
		int wa, ha, wb, hb, i, mx = 0;
		unsigned char *a = load(argv[1], argv[k], &wa, &ha), *b = load(argv[2], argv[k], &wb, &hb), *o;
		long n8 = 0, n32 = 0, n;
		double sum = 0.0;
		char p[600];
		FILE *f;

		if (!a || !b || wa != wb || ha != hb)
		{
			printf("%-28s FAIL: missing, or the sizes differ (%s %s)\n", argv[k], a ? "A ok" : "A missing", b ? "B ok" : "B missing");
			failed++;
			continue;
		}
		n = (long)wa * ha;
		o = malloc((size_t)n * 3);
		for (i = 0; i < n; i++)
		{
			int d0 = abs(a[3 * i] - b[3 * i]), d1 = abs(a[3 * i + 1] - b[3 * i + 1]), d2 = abs(a[3 * i + 2] - b[3 * i + 2]);
			int d = d0 > d1 ? d0 : d1;

			if (d2 > d)
				d = d2;
			if (d > mx)
				mx = d;
			if (d > 8)
				n8++;
			if (d > 32)
				n32++;
			sum += d0 + d1 + d2;
			o[3 * i] = (unsigned char)(d0 * 8 > 255 ? 255 : d0 * 8);
			o[3 * i + 1] = (unsigned char)(d1 * 8 > 255 ? 255 : d1 * 8);
			o[3 * i + 2] = (unsigned char)(d2 * 8 > 255 ? 255 : d2 * 8);
		}
		snprintf(p, sizeof p, "%s/%s.ppm", argv[3], argv[k]);
		f = fopen(p, "wb");
		if (f)
		{
			fprintf(f, "P6\n%d %d\n255\n", wa, ha);
			fwrite(o, 3, (size_t)n, f);
			fclose(f);
		}
		printf("%-28s %s  max %3d  mean %.3f  over 8: %6ld (%.3f%%)  over 32: %6ld (%.3f%%)\n", argv[k], n32 * 200 > n ? "FAIL" : "ok  ", mx,
			sum / (3.0 * n), n8, 100.0 * n8 / n, n32, 100.0 * n32 / n);
		if (n32 * 200 > n)
			failed++;
		free(a);
		free(b);
		free(o);
	}
	printf("%d picture(s) failed\n", failed);
	return failed;
}
