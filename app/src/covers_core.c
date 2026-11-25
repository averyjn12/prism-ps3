/* See covers_core.h. No console calls here: host/covers_test.sh builds this file for the PC. */
#include <stdlib.h>
#include <string.h>
#include "covers_core.h"

static int is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static int is_alpha(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int cv_is_id(const char *id)
{
	int i, n = id ? (int)strlen(id) : 0;

	if (n != 11 && n != 9)
		return 0;
	for (i = 0; i < 4; i++)
		if (!is_alpha(id[i]))
			return 0;
	if (n == 9)
	{
		/* v0.58: PS3 / PSP, BLUS30386 */
		for (i = 4; i < 9; i++)
			if (!is_digit(id[i]))
				return 0;
		return 1;
	}
	if (id[4] != '_' || id[8] != '.')
		return 0;
	for (i = 5; i < 11; i++)
		if (i != 8 && !is_digit(id[i]))
			return 0;
	return 1;
}

void cv_dash(const char *id, char *out)
{
	/* SLUS_212.43 -> SLUS-21243: the form the PCSX2 / DuckStation cover sets use; BLUS30386 -> BLUS-30386 */
	memcpy(out, id, 4);
	out[4] = '-';
	if (strlen(id) == 9)
	{
		memcpy(out + 5, id + 4, 5);
		out[10] = 0;
		return;
	}
	memcpy(out + 5, id + 5, 3);
	memcpy(out + 8, id + 9, 2);
	out[10] = 0;
}

int cv_expand(const char *tpl, const char *id, char *out, int n)
{
	char dash[12];
	int k = 0;

	if (!cv_is_id(id) || n < 1)
		return -1;
	cv_dash(id, dash);
	while (*tpl)
	{
		const char *put = NULL;
		int len = 1;

		if (!strncmp(tpl, "{ID}", 4))
			put = id, len = 4;
		else if (!strncmp(tpl, "{DASH}", 6))
			put = dash, len = 6;
		if (put)
		{
			int m = (int)strlen(put);

			if (k + m >= n)
				return -1;
			memcpy(out + k, put, m);
			k += m;
		}
		else
		{
			if (k + 1 >= n)
				return -1;
			out[k++] = *tpl;
		}
		tpl += len;
	}
	out[k] = 0;
	return 0;
}

static void trim(char *s)
{
	char *e = s + strlen(s);

	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		*--e = 0;
	if (*s == ' ' || *s == '\t')
	{
		char *b = s;

		while (*b == ' ' || *b == '\t')
			b++;
		memmove(s, b, strlen(b) + 1);
	}
}

/* name | platforms | where.  where: https://host/path, or /a/path on this console */
int cv_sources_parse(const char *text, cv_src *out, int max)
{
	int n = 0;

	while (*text && n < max)
	{
		char ln[400], *a, *b;
		int len = (int)strcspn(text, "\n");
		cv_src s;

		memset(&s, 0, sizeof s);
		if (len >= (int)sizeof ln)
			len = sizeof ln - 1;
		memcpy(ln, text, len);
		ln[len] = 0;
		text += strcspn(text, "\n");
		if (*text)
			text++;
		trim(ln);
		if (!ln[0] || ln[0] == '#' || !(a = strchr(ln, '|')) || !(b = strchr(a + 1, '|')))
			continue;
		*a = *b = 0;
		trim(ln);
		trim(a + 1);
		trim(b + 1);
		if (!ln[0] || strlen(ln) >= sizeof s.name)
			continue;
		strcpy(s.name, ln);
		{
			int p;

			for (p = CV_PS1; p <= CV_PSP; p++)
				if (strchr(a + 1, '0' + p))
					s.plats |= (uint8_t)(1 << p);
		}
		if (!s.plats)
			continue;
		if (!strncmp(b + 1, "game:", 5) && b[6] && strlen(b + 6) < sizeof s.path)
		{
			s.local = 2;	/* v0.58: the game's own picture, named after "game:" (ICON0.PNG) */
			strcpy(s.path, b + 6);
		}
		else if (!strncmp(b + 1, "https://", 8))
		{
			char *host = b + 9, *slash = strchr(host, '/');

			if (!slash || slash == host || slash - host >= (int)sizeof s.host || strlen(slash) >= sizeof s.path)
				continue;
			memcpy(s.host, host, slash - host);
			strcpy(s.path, slash);
		}
		else if (b[1] == '/' && strlen(b + 1) < sizeof s.path)
		{
			s.local = 1;
			strcpy(s.path, b + 1);
		}
		else
			continue;	/* plain http:// and anything else: not accepted */
		out[n++] = s;
	}
	return n;
}

int cv_scale(const uint8_t *argb, int w, int h, int pitch, uint8_t *out)
{
	int ox, oy;

	if (!argb || w < 1 || h < 1 || pitch < w * 4)
		return -1;
	for (oy = 0; oy < CV_COVER; oy++)
	{
		int y0 = (int)((long long)oy * h / CV_COVER), y1 = (int)((long long)(oy + 1) * h / CV_COVER);

		if (y1 <= y0)
			y1 = y0 + 1;
		for (ox = 0; ox < CV_COVER; ox++)
		{
			int x0 = (int)((long long)ox * w / CV_COVER), x1 = (int)((long long)(ox + 1) * w / CV_COVER), x, y;
			unsigned long r = 0, g = 0, b = 0, cnt = 0;
			uint8_t *o = out + (oy * CV_COVER + ox) * 4;

			if (x1 <= x0)
				x1 = x0 + 1;
			for (y = y0; y < y1; y++)
			{
				const uint8_t *p = argb + (long)y * pitch + x0 * 4;

				for (x = x0; x < x1; x++, p += 4)
				{
					r += p[1];
					g += p[2];
					b += p[3];
					cnt++;
				}
			}
			o[0] = (uint8_t)((r + cnt / 2) / cnt);
			o[1] = (uint8_t)((g + cnt / 2) / cnt);
			o[2] = (uint8_t)((b + cnt / 2) / cnt);
			o[3] = 0x80;
		}
	}
	return 0;
}

int cv_fit(const uint8_t *argb, int w, int h, int pitch, uint8_t *out)
{
	static uint8_t box[CV_PIX];
	unsigned long long sr = 0, sg = 0, sb = 0;
	int bw, bh, x0, y0, x, y;

	if (!argb || w < 1 || h < 1 || pitch < w * 4)
		return -1;
	/* the fill: the whole picture's average colour, at 3/8 brightness */
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			const uint8_t *p = argb + (long)y * pitch + x * 4;

			sr += p[1];
			sg += p[2];
			sb += p[3];
		}
	{
		unsigned long long n = (unsigned long long)w * h;
		uint8_t fr = (uint8_t)(sr / n * 3 / 8), fg = (uint8_t)(sg / n * 3 / 8), fb = (uint8_t)(sb / n * 3 / 8);

		for (x = 0; x < CV_COVER * CV_COVER; x++)
		{
			out[x * 4] = fr;
			out[x * 4 + 1] = fg;
			out[x * 4 + 2] = fb;
			out[x * 4 + 3] = 0x80;
		}
	}
	/* the picture's box inside the square, its shape kept */
	if (w >= h)
	{
		bw = CV_COVER;
		bh = (int)(((long long)CV_COVER * h + w / 2) / w);
	}
	else
	{
		bh = CV_COVER;
		bw = (int)(((long long)CV_COVER * w + h / 2) / h);
	}
	if (bw < 1)
		bw = 1;
	if (bh < 1)
		bh = 1;
	x0 = (CV_COVER - bw) / 2;
	y0 = (CV_COVER - bh) / 2;
	/* scale into a bw x bh area, by the same area average as cv_scale */
	for (y = 0; y < bh; y++)
	{
		int sy0 = (int)((long long)y * h / bh), sy1 = (int)((long long)(y + 1) * h / bh);

		if (sy1 <= sy0)
			sy1 = sy0 + 1;
		for (x = 0; x < bw; x++)
		{
			int sx0 = (int)((long long)x * w / bw), sx1 = (int)((long long)(x + 1) * w / bw), i, j;
			unsigned long r = 0, g = 0, b = 0, cnt = 0;
			uint8_t *o = box + (y * bw + x) * 4;

			if (sx1 <= sx0)
				sx1 = sx0 + 1;
			for (j = sy0; j < sy1; j++)
				for (i = sx0; i < sx1; i++)
				{
					const uint8_t *p = argb + (long)j * pitch + i * 4;

					r += p[1];
					g += p[2];
					b += p[3];
					cnt++;
				}
			o[0] = (uint8_t)((r + cnt / 2) / cnt);
			o[1] = (uint8_t)((g + cnt / 2) / cnt);
			o[2] = (uint8_t)((b + cnt / 2) / cnt);
		}
	}
	for (y = 0; y < bh; y++)
		for (x = 0; x < bw; x++)
		{
			uint8_t *o = out + ((y0 + y) * CV_COVER + x0 + x) * 4;
			const uint8_t *s = box + (y * bw + x) * 4;

			o[0] = s[0];
			o[1] = s[1];
			o[2] = s[2];
		}
	return 0;
}

static int num_after(const char *t, const char *key)
{
	const char *p = strstr(t, key);

	return p ? atoi(p + strlen(key)) : -1;
}

int cv_art_parse(const uint8_t *buf, long len, cv_entry *out, int max)
{
	char head[CV_SECTOR + 1];
	int n, data, i, k = 0;

	if (!buf || len < 2 * CV_SECTOR)
		return -1;
	memcpy(head, buf, CV_SECTOR);
	head[CV_SECTOR] = 0;
	if (strncmp(head, "PRISMART1\n", 10) || num_after(head, "\nw=") != CV_COVER || num_after(head, "\nh=") != CV_COVER)
		return -2;
	n = num_after(head, "\ncount=");
	data = num_after(head, "\ndata=");
	if (n < 0 || data < 2 || (long)CV_SECTOR + 128L * n > len || 128L * n > (long)(data - 1) * CV_SECTOR)
		return -3;
	for (i = 0; i < n && k < max; i++)
	{
		const uint8_t *r = buf + CV_SECTOR + 128 * i;
		int slot = r[16] | (r[17] << 8);
		long off = (long)data * CV_SECTOR + (long)slot * CV_PIX;
		cv_entry *e = &out[k];

		memset(e, 0, sizeof *e);
		memcpy(e->id, r, 11);
		if (!cv_is_id(e->id))
			continue;
		memcpy(e->title, r + 20, 107);
		e->plat = (r[18] & 0x7F) >= CV_PS1 && (r[18] & 0x7F) <= CV_PSP ? r[18] & 0x7F : 0;
		e->refused = r[18] & CV_REFUSED;
		e->px = slot != 0xFFFF && off + CV_PIX <= len ? buf + off : NULL;
		k++;
	}
	return k;
}

int cv_art_write(const cv_entry *e, int n, FILE *f)
{
	static const uint8_t zero[CV_SECTOR];
	char head[CV_SECTOR];
	int *order, n_ord = 0, pass, i, slot = 0;

	if (!f || n < 0)
		return -1;
	order = (int *)malloc(sizeof(int) * (n ? n : 1));
	if (!order)
		return -2;
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < n && n_ord < CV_REC_MAX; i++)
			if ((pass == 0) == (e[i].plat == CV_PS2 || e[i].plat == 0) && cv_is_id(e[i].id))
				order[n_ord++] = i;
	memset(head, 0, sizeof head);
	snprintf(head, sizeof head, "PRISMART1\ncount=%d\nw=%d\nh=%d\ndata=%d\n", n_ord, CV_COVER, CV_COVER, CV_DATA_SEC);
	if (fwrite(head, 1, CV_SECTOR, f) != CV_SECTOR)
		goto bad;
	for (i = 0; i < n_ord; i++)
	{
		const cv_entry *x = &e[order[i]];
		uint8_t r[128];
		int s = 0xFFFF;

		memset(r, 0, sizeof r);
		memcpy(r, x->id, 11);
		if (x->px && slot < CV_COV_MAX)
			s = slot++;
		r[16] = (uint8_t)(s & 0xFF);
		r[17] = (uint8_t)(s >> 8);
		r[18] = (uint8_t)(x->plat | (x->refused ? CV_REFUSED : 0));
		strncpy((char *)r + 20, x->title, 107);
		if (fwrite(r, 1, 128, f) != 128)
			goto bad;
	}
	/* pad the records out to the data sector */
	{
		long at = CV_SECTOR + 128L * n_ord, to = (long)CV_DATA_SEC * CV_SECTOR;

		while (at < to)
		{
			long m = to - at > CV_SECTOR ? CV_SECTOR : to - at;

			if (fwrite(zero, 1, m, f) != (size_t)m)
				goto bad;
			at += m;
		}
	}
	for (i = 0, slot = 0; i < n_ord; i++)
	{
		const cv_entry *x = &e[order[i]];

		if (!x->px || slot >= CV_COV_MAX)
			continue;
		if (fwrite(x->px, 1, CV_PIX, f) != CV_PIX)
			goto bad;
		slot++;
	}
	free(order);
	return slot;
bad:
	free(order);
	return -3;
}

int cv_cover(const uint8_t *argb, int w, int h, int pitch, uint8_t *out)
{
	/* a PSP icon is 144 x 80, a PS3 one 320 x 176; box art is taller than wide, PSBBN's square */
	if ((long long)w * 100 > (long long)h * 115)
		return cv_fit(argb, w, h, pitch, out);
	return cv_scale(argb, w, h, pitch, out);
}

int cv_src_folder(const cv_src *s, char *out, int n)
{
	const char *e;

	if (s->local != 1)
		return 0;	/* v0.59: 0.58 asked this of game: sources too, found no folder, and never offered them */
	e = strrchr(s->path, '/');
	if (!e || e == s->path || e - s->path >= n || memchr(s->path, '{', e - s->path))
		return 0;
	memcpy(out, s->path, e - s->path);
	out[e - s->path] = 0;
	return 1;
}

int cv_beside(const char *game_path, int k, char *out, int n)
{
	static const char *const ext[4] = { ".png", ".jpg", ".PNG", ".JPG" };
	const char *slash = strrchr(game_path, '/'), *dot = strrchr(game_path, '.');
	int base;

	if (k < 0 || k > 3 || strncmp(game_path, "/dev_", 5) || !slash || !dot || dot < slash || dot == slash + 1)
		return -1;
	base = (int)(dot - game_path);
	if (base + 5 > n)	/* the name, a 4-letter extension, the NUL */
		return -1;
	memcpy(out, game_path, base);
	strcpy(out + base, ext[k]);
	return 0;
}
