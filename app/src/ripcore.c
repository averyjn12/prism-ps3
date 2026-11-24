/* v0.38: copying a PS1 / PS2 disc, the portable half (ripcore.h). The console half - the calls that
 * reach the drive - is drive.c; host/ripcore_test.c runs this file against a pretend drive.
 *
 * What the track list and the cue sheet look like follows the Cobra library, the code multiMAN's
 * "Create ISO" runs (ManaGunZ @ 4a11fa4, MGZ/lib/cobra/source/cobra.c: cobra_get_cd_td L973-1087,
 * cobra_create_cue L1384-1409). */
#include <stdio.h>
#include <string.h>
#include "ripcore.h"

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return be16(p) << 16 | be16(p + 2); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int no(char *why, int wn, const char *s)
{
	if (why && wn > 0)
		snprintf(why, wn, "%s", s);
	return -1;
}

int rc_toc_parse(const uint8_t *r, int len, rc_toc *out, char *why, int wn)
{
	int first, last, nd, i, j, lead = 0;

	memset(out, 0, sizeof *out);
	if (len < 4)
		return no(why, wn, "the disc's track list is too short");
	nd = ((int)be16(r) - 2) / 8;
	first = r[2];
	last = r[3];
	if (nd < 0 || 4 + nd * 8 > len)
		return no(why, wn, "the disc's track list is cut short");
	if (first != 1 || last < 1 || last > RC_MAX_TRACKS)
		return no(why, wn, "the disc's track numbers are not 1 .. 99");
	for (i = 1; i <= last; i++)
	{
		for (j = 0; j < nd; j++)
			if (r[4 + j * 8 + 2] == i)
				break;
		if (j == nd)
			return no(why, wn, "a track is missing from the disc's track list");
		{
			/* cobra_get_cd_td: control bits 0, 1, 8, 9 (after & 0xD) are audio */
			int ct = r[4 + j * 8 + 1] & 0xD;

			out->t[i - 1].audio = (uint8_t)(ct == 0 || ct == 1 || ct == 8 || ct == 9);
			out->t[i - 1].lba = be32(r + 4 + j * 8 + 4);
		}
	}
	for (j = 0; j < nd; j++)
		if (r[4 + j * 8 + 2] == 0xAA)
		{
			out->leadout = be32(r + 4 + j * 8 + 4);
			lead = 1;
			break;
		}
	if (!lead)
		return no(why, wn, "the disc's track list has no end mark");
	out->n = last;
	/* what Prism copies: a game disc - data first, from sector 0, and only music after it. (Reading
	 * with the wrong kind for a sector asks the drive for a length it will not send.) */
	if (out->t[0].audio || out->t[0].lba != 0)
		return no(why, wn, "the disc does not start with a data track (not a game disc)");
	for (i = 1; i < out->n; i++)
	{
		if (!out->t[i].audio)
			return no(why, wn, "the disc has a second data track: Prism does not copy that layout");
		if (out->t[i].lba <= out->t[i - 1].lba)
			return no(why, wn, "the disc's tracks are out of order");
	}
	if (out->leadout <= out->t[out->n - 1].lba || out->leadout > 450000u)
		return no(why, wn, "the disc's end mark is in the wrong place");
	return 0;
}

int rc_audio_at(const rc_toc *toc, uint32_t lba)
{
	int i, a = 0;

	for (i = 0; i < toc->n && toc->t[i].lba <= lba; i++)
		a = toc->t[i].audio;
	return a;
}

int rc_cue(const rc_toc *toc, const char *bin, char *out, int n)
{
	int k, i;

	k = snprintf(out, n, "FILE \"%s\" BINARY\r\n", bin);
	for (i = 0; i < toc->n && k > 0 && k < n; i++)
	{
		uint32_t lba = toc->t[i].lba - toc->t[0].lba;

		k += snprintf(out + k, n - k, "  TRACK %02d %s\r\n    INDEX 01 %02u:%02u:%02u\r\n", i + 1,
			toc->t[i].audio ? "AUDIO" : "MODE2/2352", (unsigned)(lba / 4500), (unsigned)(lba / 75 % 60), (unsigned)(lba % 75));
	}
	return k > 0 && k < n ? k : -1;
}

int rc_user_off(const uint8_t *raw)
{
	static const uint8_t sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };

	if (memcmp(raw, sync, 12))
		return -1;
	return raw[15] == 2 ? 24 : raw[15] == 1 ? 16 : -1;
}

static uint8_t bcd(uint32_t v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

int rc_data_ok(const uint8_t *raw, uint32_t lba)
{
	uint32_t m = lba + 150;

	return rc_user_off(raw) >= 0 && raw[12] == bcd(m / 4500) && raw[13] == bcd(m / 75 % 60) && raw[14] == bcd(m % 75);
}

/* ---- the title ID ---- */

static int cnf_boot(char *d, char *id, int idn)
{
	char *p, *q;
	int k = 0, ps2;

	id[0] = 0;
	ps2 = strstr(d, "BOOT2") != NULL;
	p = strstr(d, ps2 ? "cdrom0:" : "cdrom:");
	if (!p)
		return -5;
	p += ps2 ? 7 : 6;
	while (*p == '\\' || *p == '/')
		p++;
	for (q = p; *q > ' ' && *q != ';' && k < idn - 1 && k < 11; q++)
		id[k++] = *q;
	id[k] = 0;
	return ps2;
}

int rc_disc_id(rc_rd2048 rd, void *ctx, char *id, int idn)
{
	uint8_t b[2049];
	uint32_t root, rsz, s, i;

	id[0] = 0;
	if (rd(ctx, 16, b) || b[0] != 1 || memcmp(b + 1, "CD001", 5))
		return -1;
	root = le32(b + 158);
	rsz = le32(b + 166);
	for (s = 0; s < 8 && s * 2048 < rsz; s++)
	{
		if (rd(ctx, root + s, b))
			return -2;
		for (i = 0; i + 44 < 2048 && b[i]; i += b[i])
			if (b[i + 32] >= 10 && !memcmp(b + i + 33, "SYSTEM.CNF", 10))
			{
				if (rd(ctx, le32(b + i + 2), b))
					return -4;
				b[2048] = 0;
				return cnf_boot((char *)b, id, idn);
			}
	}
	return -3;
}

void rc_name(char *out, int max, const char *title, const char *id, const char *fallback)
{
	char t[80];
	int k = 0, sp = 1;
	const char *p;

	for (p = title ? title : ""; *p && k < 60; p++)
	{
		unsigned char c = (unsigned char)*p;

		if (c < 32 || c > 126 || strchr("\\/:*?\"<>|%+", c))
			c = ' ';	/* (webMAN mounts a '+' as %2B; '%' breaks its paths) */
		if (c == ' ' && sp)
			continue;
		sp = c == ' ';
		t[k++] = (char)c;
	}
	while (k && (t[k - 1] == ' ' || t[k - 1] == '.'))
		k--;
	t[k] = 0;
	if (k && id && id[0])
		snprintf(out, max, "%s (%s)", t, id);
	else if (k)
		snprintf(out, max, "%s", t);
	else if (id && id[0])
		snprintf(out, max, "%s", id);
	else
		snprintf(out, max, "%s", fallback);
}

/* ---- the copy loops ---- */

/* Two seconds (150 sectors) before a track start and before the disc's end, and a few more: on
 * Bananafuzzbuddy's "Loaded" disc (11-23) the one sector that would not read was 151 before track 2, the last
 * of the data track, where the disc changes from data to music. */
#define GAP		156u
#define GAP_GIVE_UP	8u	/* this many gap sectors in a row unread: the rest of that gap is left blank */
#define CD_TRIES	3	/* the Cobra library tries 10 times, IRISMAN 5; each failed try can take seconds */
#define MAX_BAD		200u	/* more than this outside the gaps: the disc is not being read */
#define MAX_RUN		24u	/* this many in a row before anything at all was read: the same */
#define FAST_FAIL_RUN	8u	/* this many many-sector commands failed in a row: the rest goes one by one */
#define FAST_WRONG_MAX	64u	/* more data sectors than this came wrong from them: the same */

static void say(const rc_io *io, const char *fmt, uint32_t a, uint32_t b)
{
	char ln[120];

	if (!io->log)
		return;
	snprintf(ln, sizeof ln, fmt, (unsigned)a, (unsigned)b);
	io->log(io->ctx, ln);
}

/* 1 = lba is in the two seconds before a track start (from the second track on) or before the end */
static int in_gap(const rc_toc *toc, uint32_t lba)
{
	int i;

	if (lba + GAP >= toc->leadout)
		return 1;
	for (i = 1; i < toc->n; i++)
		if (lba < toc->t[i].lba && lba + GAP >= toc->t[i].lba)
			return 1;
	return 0;
}

/* where the gap that lba is in ends: the next track's start, or the disc's end */
static uint32_t gap_end(const rc_toc *toc, uint32_t lba)
{
	int i;

	for (i = 1; i < toc->n; i++)
		if (lba < toc->t[i].lba && lba + GAP >= toc->t[i].lba)
			return toc->t[i].lba;
	return toc->leadout;
}

int rc_rip_cd(const rc_toc *toc, const rc_io *io, uint8_t *buf, uint32_t chunk, rc_stats *st)
{
	uint32_t lba = 0, total = toc->leadout, run = 0;
	uint32_t gap_run = 0, skip_to = 0;	/* a gap that does not read is not asked for sector by sector */
	int good = 0;		/* something has been read */
	uint64_t bytes = (uint64_t)total * RC_RAW;
	static uint8_t have[RC_CHUNK_MAX];	/* this block's sectors that the fast read gave */
	int fast = io->read_cd_n != NULL;
	uint32_t fail_run = 0;

	memset(st, 0, sizeof *st);
	if (!chunk || chunk % 32 || chunk > RC_CHUNK_MAX || !total)
		return RC_EARG;
	while (lba < total)
	{
		uint32_t n = total - lba < chunk ? total - lba : chunk, k, m = 0;

		/* v0.44, the fast way: the block (or, when music may not come this way, its data sectors)
		 * with one command. What it gives is kept only where it can be trusted: music as it comes
		 * (as IRISMAN keeps it), a data sector when it is the sector that belongs there. Everything
		 * else in the block goes through the loop below exactly as before. */
		memset(have, 0, n);
		if (fast)
			for (m = 0; m < n && (io->fast_audio || !rc_audio_at(toc, lba + m)); m++)
				;
		if (m)
		{
			if (io->read_cd_n(io->ctx, lba, m, buf) == 0)
			{
				fail_run = 0;
				for (k = 0; k < m; k++)
				{
					const uint8_t *q = buf + (size_t)k * RC_RAWQ;
					uint32_t s = lba + k;

					have[k] = (uint8_t)(rc_audio_at(toc, s) || rc_data_ok(q, s));
					if (k)
						memmove(buf + (size_t)k * RC_RAW, q, RC_RAW);	/* the sub-channel bytes drop out */
					if (have[k])
						st->fast++;
					else if (!in_gap(toc, s) && st->fast_wrong++ < 4)
						say(io, "sector %u came wrong from the fast read (%u so far): read again by itself", s, st->fast_wrong);
				}
			}
			else
			{
				fail_run++;
				if (st->fast_fail++ < 4)
					say(io, "the %u sectors at %u would not read with one command: one at a time", m, lba);
			}
			if (fail_run >= FAST_FAIL_RUN || st->fast_wrong > FAST_WRONG_MAX)
			{
				fast = 0;
				st->fast_off_at = lba + 1;
				say(io, "the fast read is given up from sector %u on (%u commands failed so far)", lba, st->fast_fail);
			}
		}
		for (k = 0; k < n; k++)
		{
			uint32_t s = lba + k;
			uint8_t *p = buf + (size_t)k * RC_RAW;
			int audio = rc_audio_at(toc, s), gap = in_gap(toc, s), tries = gap ? 1 : CD_TRIES, t, ok = 0;

			if (have[k])
			{
				run = gap_run = 0;
				good = 1;
				continue;
			}
			if (gap && s < skip_to)
			{
				memset(p, 0, RC_RAW);
				st->gap_bad++;
				continue;
			}
			for (t = 0; t < tries && !ok; t++)
			{
				ok = io->read_cd(io->ctx, s, audio, p) == 0;
				if (ok && t)
					st->retries++;
			}
			if (ok)
			{
				run = gap_run = 0;
				good = 1;
				continue;
			}
			memset(p, 0, RC_RAW);
			if (gap)
			{
				st->gap_bad++;
				if (++gap_run >= GAP_GIVE_UP)
				{
					skip_to = gap_end(toc, s);
					gap_run = 0;
				}
			}
			else
			{
				if (!st->bad)
					st->first_bad = s;
				st->bad++;
				run++;
				if (st->bad <= 8)
					say(io, "sector %u could not be read (%u so far): blank in the copy", s, st->bad);
			}
			if (io->present && !io->present(io->ctx))
				return RC_GONE;
			if (st->bad > MAX_BAD || (run >= MAX_RUN && !good))
				return RC_EREAD;
		}
		if (io->write(io->ctx, buf, n * RC_RAW))
			return RC_EWRITE;
		lba += n;
		st->done += (uint64_t)n * RC_RAW;
		if (io->tick && io->tick(io->ctx, st->done, bytes))
			return RC_STOP;
	}
	return RC_OK;
}

#define DVD_TRIES	3
#define DVD_ONE_TRIES	5	/* ManaGunZ's DUMPER_read: the block again, then sector by sector */

int rc_rip_dvd(uint32_t sectors, const rc_io *io, uint8_t *buf, uint32_t chunk, rc_stats *st)
{
	uint32_t lba = 0, run = 0;
	int good = 0;
	uint64_t bytes = (uint64_t)sectors * 2048;

	memset(st, 0, sizeof *st);
	if (!chunk || !sectors)
		return RC_EARG;
	while (lba < sectors)
	{
		uint32_t n = sectors - lba < chunk ? sectors - lba : chunk, k;
		int t, ok = 0;

		for (t = 0; t < DVD_TRIES && !ok; t++)
		{
			ok = io->read_2048(io->ctx, lba, n, buf) == 0;
			if (ok && t)
				st->retries++;
		}
		if (ok)
		{
			run = 0;
			good = 1;
		}
		else
		{
			if (io->present && !io->present(io->ctx))
				return RC_GONE;
			say(io, "the %u sectors at %u would not read together: one at a time", n, lba);
			for (k = 0; k < n; k++)
			{
				uint8_t *p = buf + (size_t)k * 2048;

				ok = 0;
				for (t = 0; t < DVD_ONE_TRIES && !ok; t++)
					ok = io->read_2048(io->ctx, lba + k, 1, p) == 0;
				if (ok)
				{
					run = 0;
					good = 1;
					continue;
				}
				memset(p, 0, 2048);
				if (!st->bad)
					st->first_bad = lba + k;
				st->bad++;
				run++;
				if (st->bad <= 8)
					say(io, "sector %u could not be read (%u so far): blank in the copy", lba + k, st->bad);
				if (io->present && !io->present(io->ctx))
					return RC_GONE;
				if (st->bad > MAX_BAD || (run >= MAX_RUN && !good))
					return RC_EREAD;
			}
		}
		if (io->write(io->ctx, buf, n * 2048))
			return RC_EWRITE;
		lba += n;
		st->done += (uint64_t)n * 2048;
		if (io->tick && io->tick(io->ctx, st->done, bytes))
			return RC_STOP;
	}
	return RC_OK;
}

/* ---- a file in parts ---- */

int rc_parts_begin(rc_parts *p, uint64_t total, uint64_t part_max, uint64_t whole_max)
{
	p->total = total;
	p->part_max = part_max;
	p->done = p->in_part = 0;
	p->part = p->is_open = p->parts = 0;
	p->split = whole_max && total > whole_max;
	if (!part_max || part_max % 2048)
		return RC_EARG;
	if (p->split && (total + part_max - 1) / part_max > RC_PARTS_MAX)
		return RC_EARG;
	return 0;
}

int rc_parts_write(rc_parts *p, const uint8_t *buf, uint32_t len)
{
	while (len)
	{
		uint64_t room = p->split ? p->part_max - p->in_part : (uint64_t)-1;
		uint32_t n = room < len ? (uint32_t)room : len;

		if (!p->is_open)
		{
			if (p->open(p->ctx, p->split ? p->part : -1))
				return RC_EWRITE;
			p->is_open = 1;
			p->parts++;
			p->in_part = 0;
			continue;	/* room is this part's, now */
		}
		if (!n)
		{
			/* the part is full: the next byte opens the next one */
			if (p->close(p->ctx))
				return RC_EWRITE;
			p->is_open = 0;
			p->part++;
			continue;
		}
		if (p->write(p->ctx, buf, n))
			return RC_EWRITE;
		buf += n;
		len -= n;
		p->in_part += n;
		p->done += n;
	}
	return 0;
}

int rc_parts_end(rc_parts *p)
{
	int r = 0;

	if (p->is_open)
	{
		r = p->close(p->ctx) ? RC_EWRITE : 0;
		p->is_open = 0;
	}
	return r ? r : p->done == p->total ? 0 : RC_EWRITE;
}
