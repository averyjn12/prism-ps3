/* v0.38: the disc-copy core (src/ripcore.c) against a pretend drive, for host/ripcore_test.sh.
 *   ripcore_test unit <game.iso>                  the scenarios below; prints PASS / FAIL lines
 *   ripcore_test rip <game.iso> <outdir>          a pretend CD (the ISO as a data track + 3 music
 *                                                 tracks) copied to <outdir>/game.bin + game.cue,
 *                                                 with the music as it must come out in trackNN.raw
 *   ripcore_test exfat <image> <folder> <name> <game.iso> cd|dvd
 *                                                 the same copy (or the ISO as a DVD) written through
 *                                                 the exFAT writer into an image; expect.bin beside it
 * The pretend drive is strict where the real one is dangerous: a read past the disc's end, or a data
 * sector asked for with the audio flag (the real drive would answer with fewer bytes than asked: the
 * suspected v0.32 freeze), is counted in `fatal` and fails the run. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ripcore.h"
#include "exfat_w.h"

static int pass, fail;
#define CHECK(c, ...)	do { if (c) { pass++; printf("PASS  "); } else { fail++; printf("FAIL  "); } printf(__VA_ARGS__); printf("\n"); } while (0)

/* ---- the pretend disc ---- */
static uint8_t *iso;
static uint32_t iso_sectors;
static rc_toc dtoc;			/* its real layout */
static uint32_t data_end;		/* data sectors: 0 .. data_end; then pregap; then audio */
static uint8_t *fail_left;		/* per sector: reads that still fail (255 = always) */
static uint32_t fatal, reads, past_end, audio_on_data, tries_max;
static uint8_t *tries;
static int gone_after = -1, stop_at_pct = -1, write_fail_at = -1, exfat_out;
static uint8_t *out;
static uint64_t out_len, out_cap;
static uint32_t odd_writes, writes;
static uint64_t last_write_len;

static uint8_t bcd(uint32_t v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

static void raw_data(uint32_t lba, uint8_t *b)
{
	uint32_t m = lba + 150, i;

	memset(b, 0xFF, 12);
	b[0] = b[11] = 0;
	b[12] = bcd(m / 4500);
	b[13] = bcd(m / 75 % 60);
	b[14] = bcd(m % 75);
	b[15] = 2;
	b[16] = b[20] = 0;
	b[17] = b[21] = 0;
	b[18] = b[22] = 8;
	b[19] = b[23] = 0;
	memcpy(b + 24, iso + (size_t)lba * 2048, 2048);
	for (i = 2072; i < 2352; i++)
		b[i] = (uint8_t)(lba * 7 + i);	/* stands for EDC / ECC */
}

static void raw_audio(uint32_t lba, uint8_t *b)
{
	uint32_t i;

	if (lba < dtoc.t[1].lba)
	{
		memset(b, 0, 2352);	/* the pregap: silence */
		return;
	}
	for (i = 0; i < 2352; i++)
		b[i] = (uint8_t)((lba * 2654435761u + i * 40503u) >> 13);
}

static void disc_sector(uint32_t lba, uint8_t *b)
{
	if (lba < data_end)
		raw_data(lba, b);
	else
		raw_audio(lba, b);
}

static int d_read_cd(void *ctx, uint32_t lba, int audio, uint8_t *b)
{
	(void)ctx;
	reads++;
	if (lba >= dtoc.leadout)
	{
		past_end++;
		fatal++;
		return -1;
	}
	if (audio && lba < data_end)
	{
		audio_on_data++;
		fatal++;
		return -1;
	}
	if (tries[lba] < 255)
		tries[lba]++;
	if (tries[lba] > tries_max)
		tries_max = tries[lba];
	if (fail_left[lba])
	{
		if (fail_left[lba] != 255)
			fail_left[lba]--;
		return -1;
	}
	disc_sector(lba, b);
	return 0;
}

/* v0.44, the fast read: n sectors with one command, 16 sub-channel bytes after each. A command over a
 * sector that does not read fails whole and leaves the buffer as it was (stale bytes). */
static uint32_t fast_cmds, fast_secs, fast_music_secs;
static int fast_fail_at = -1;		/* a command touching this sector fails, though single reads work */
static int fast_wrong_at = -1;		/* this sector comes back as its neighbour (a good sector, the wrong one) */
static int fast_fail_all, fast_wrong_all;

static int d_read_cd_n(void *ctx, uint32_t lba, uint32_t n, uint8_t *b)
{
	uint32_t k;

	(void)ctx;
	fast_cmds++;
	if (!n || n > 1024 || lba + n > dtoc.leadout)
	{
		past_end++;
		fatal++;
		return -1;
	}
	if (fast_fail_all || (fast_fail_at >= 0 && (uint32_t)fast_fail_at >= lba && (uint32_t)fast_fail_at < lba + n))
		return -1;
	for (k = 0; k < n; k++)
		if (fail_left[lba + k])
			return -1;
	for (k = 0; k < n; k++)
	{
		uint8_t *p = b + (size_t)k * 2368;
		uint32_t s = lba + k;

		disc_sector((fast_wrong_all || (int)s == fast_wrong_at) && s + 1 < data_end ? s + 1 : s, p);
		memset(p + 2352, 0xA5, 16);
		fast_secs++;
		fast_music_secs += dtoc.n > 1 && s >= dtoc.t[1].lba;
	}
	return 0;
}

static int multi_fail_lba = -1;		/* a block read touching this sector fails; a single read works */

static int d_read_2048(void *ctx, uint32_t lba, uint32_t n, uint8_t *b)
{
	uint32_t k;

	(void)ctx;
	reads++;
	if (lba + n > iso_sectors)
	{
		past_end++;
		fatal++;
		return -1;
	}
	if (n > 1 && multi_fail_lba >= 0 && (uint32_t)multi_fail_lba >= lba && (uint32_t)multi_fail_lba < lba + n)
		return -1;
	for (k = 0; k < n; k++)
	{
		if (tries[lba + k] < 255)
			tries[lba + k]++;
		if (tries[lba + k] > tries_max)
			tries_max = tries[lba + k];
		if (fail_left[lba + k])
		{
			if (fail_left[lba + k] != 255)
				fail_left[lba + k]--;
			return -1;
		}
	}
	memcpy(b, iso + (size_t)lba * 2048, (size_t)n * 2048);
	return 0;
}

static int d_write(void *ctx, const uint8_t *b, uint32_t len)
{
	(void)ctx;
	if (last_write_len % 512)
		odd_writes++;		/* a write after one that was not whole sectors */
	last_write_len = len;
	writes++;
	if (write_fail_at >= 0 && (int)writes > write_fail_at)
		return -1;
	if (exfat_out)
		return exw_write(b, len);
	if (out_len + len > out_cap)
	{
		fatal++;
		return -1;
	}
	memcpy(out + out_len, b, len);
	out_len += len;
	return 0;
}

static int d_tick(void *ctx, uint64_t done, uint64_t total)
{
	(void)ctx;
	return stop_at_pct >= 0 && done * 100 >= total * (uint64_t)stop_at_pct;
}

static int d_present(void *ctx)
{
	(void)ctx;
	return !(gone_after >= 0 && (int)reads > gone_after);
}

static void d_log(void *ctx, const char *line)
{
	(void)ctx;
	(void)line;
}

static const rc_io io = { d_read_cd, d_read_2048, d_write, d_tick, d_present, d_log, NULL, NULL, 0 };

static void make_disc(int audio_tracks)
{
	uint32_t at;
	int i;

	memset(&dtoc, 0, sizeof dtoc);
	data_end = iso_sectors;
	dtoc.n = 1 + audio_tracks;
	dtoc.t[0].lba = 0;
	at = data_end + 150;
	for (i = 1; i <= audio_tracks; i++)
	{
		dtoc.t[i].lba = at;
		dtoc.t[i].audio = 1;
		at += 700 + 311 * i;
	}
	dtoc.leadout = audio_tracks ? at : data_end;
}

static void reset(void)
{
	free(fail_left);
	free(tries);
	free(out);
	fail_left = calloc(dtoc.leadout + iso_sectors + 1, 1);
	tries = calloc(dtoc.leadout + iso_sectors + 1, 1);
	out_cap = (uint64_t)(dtoc.leadout > iso_sectors ? dtoc.leadout : iso_sectors) * 2352 + 4096;
	out = malloc(out_cap);
	out_len = 0;
	fatal = reads = past_end = audio_on_data = tries_max = odd_writes = writes = 0;
	last_write_len = 0;
	gone_after = stop_at_pct = write_fail_at = multi_fail_lba = -1;
	fast_cmds = fast_secs = fast_music_secs = 0;
	fast_fail_at = fast_wrong_at = -1;
	fast_fail_all = fast_wrong_all = 0;
}

/* the READ TOC answer a drive gives for dtoc (format 0, LBA) */
static int toc_bytes(uint8_t *r, int with_leadout)
{
	int n = 0, i;

	r[2] = 1;
	r[3] = (uint8_t)dtoc.n;
	for (i = 0; i <= dtoc.n; i++)
	{
		uint8_t *e = r + 4 + n * 8;
		uint32_t lba = i < dtoc.n ? dtoc.t[i].lba : dtoc.leadout;

		if (i == dtoc.n && !with_leadout)
			break;
		memset(e, 0, 8);
		e[1] = i < dtoc.n ? (dtoc.t[i].audio ? 0x10 : 0x14) : 0x14;
		e[2] = i < dtoc.n ? (uint8_t)(i + 1) : 0xAA;
		e[4] = (uint8_t)(lba >> 24);
		e[5] = (uint8_t)(lba >> 16);
		e[6] = (uint8_t)(lba >> 8);
		e[7] = (uint8_t)lba;
		n++;
	}
	r[0] = (uint8_t)((n * 8 + 2) >> 8);
	r[1] = (uint8_t)(n * 8 + 2);
	return 4 + n * 8;
}

/* the copy as it must be: every sector of the disc, blank where the drive never gave one */
static int expect_same(const uint8_t *blank, const char *what)
{
	uint32_t s, bad = 0;
	uint8_t e[2352];

	if (out_len != (uint64_t)dtoc.leadout * 2352)
	{
		printf("      %s: %llu bytes, wanted %llu\n", what, (unsigned long long)out_len, (unsigned long long)dtoc.leadout * 2352);
		return 0;
	}
	for (s = 0; s < dtoc.leadout; s++)
	{
		if (blank && blank[s])
			memset(e, 0, 2352);
		else
			disc_sector(s, e);
		if (memcmp(e, out + (size_t)s * 2352, 2352) && bad++ < 3)
			printf("      %s: sector %u differs\n", what, s);
	}
	return bad == 0;
}

static int rd_iso(void *ctx, uint32_t lba, uint8_t *b)
{
	(void)ctx;
	if (lba >= iso_sectors)
		return -1;
	memcpy(b, iso + (size_t)lba * 2048, 2048);
	return 0;
}

static int rd_raw(void *ctx, uint32_t lba, uint8_t *b)
{
	uint8_t raw[2352];
	int off;

	(void)ctx;
	if (lba >= data_end)
		return -1;
	raw_data(lba, raw);
	off = rc_user_off(raw);
	if (off < 0)
		return -1;
	memcpy(b, raw + off, 2048);
	return 0;
}

static void unit(const char *want_id)
{
	static uint8_t buf[64 * 2352], dbuf[256 * 2048];
	uint8_t r[900], *blank;
	rc_toc t;
	rc_stats st;
	char why[160], cue[2048], id[16], nm[120];
	int len, rr, k;
	uint32_t s, gaps;

	/* --- the track list --- */
	make_disc(0);
	len = toc_bytes(r, 1);
	rr = rc_toc_parse(r, len, &t, why, sizeof why);
	CHECK(rr == 0 && t.n == 1 && !t.t[0].audio && t.leadout == dtoc.leadout, "track list: one data track (%d bytes) -> 1 track, end %u", len, t.leadout);
	make_disc(3);
	len = toc_bytes(r, 1);
	rr = rc_toc_parse(r, len, &t, why, sizeof why);
	CHECK(rr == 0 && t.n == 4 && !memcmp(&t, &dtoc, sizeof(rc_track) * 4) && t.leadout == dtoc.leadout, "track list: data + 3 music tracks");
	CHECK(rc_audio_at(&t, 0) == 0 && rc_audio_at(&t, dtoc.t[1].lba - 1) == 0 && rc_audio_at(&t, dtoc.t[1].lba) == 1
		&& rc_audio_at(&t, dtoc.leadout - 1) == 1, "track kinds by sector: data up to the music's first sector");
	rr = rc_toc_parse(r, toc_bytes(r, 0), &t, why, sizeof why);
	CHECK(rr < 0, "track list without an end mark refused: %s", why);
	len = toc_bytes(r, 1);
	rr = rc_toc_parse(r, len - 8, &t, why, sizeof why);
	CHECK(rr < 0, "track list cut short refused: %s", why);
	rr = rc_toc_parse(r, 3, &t, why, sizeof why);
	CHECK(rr < 0, "3-byte answer refused: %s", why);
	r[2] = 2;
	rr = rc_toc_parse(r, len, &t, why, sizeof why);
	CHECK(rr < 0, "first track number 2 refused: %s", why);
	toc_bytes(r, 1);
	r[4 + 1] = 0x10;
	rr = rc_toc_parse(r, len, &t, why, sizeof why);
	CHECK(rr < 0, "a disc that starts with music refused: %s", why);
	toc_bytes(r, 1);
	r[4 + 16 + 1] = 0x14;
	rr = rc_toc_parse(r, len, &t, why, sizeof why);
	CHECK(rr < 0, "a second data track refused: %s", why);
	toc_bytes(r, 1);
	r[4 + 8 + 7] ^= 0xFF;
	r[4 + 8 + 4] = 0x7F;
	rr = rc_toc_parse(r, len, &t, why, sizeof why);
	CHECK(rr < 0, "tracks out of order refused: %s", why);
	toc_bytes(r, 1);
	r[4 + 7] = 5;
	rr = rc_toc_parse(r, len, &t, why, sizeof why);
	CHECK(rr < 0, "a first track that does not start at 0 refused: %s", why);

	/* --- the cue sheet (bchunk reads the real one in ripcore_test.sh) --- */
	memset(&t, 0, sizeof t);
	t.n = 3;
	t.t[1].lba = 4500 * 12 + 75 * 34 + 56;
	t.t[1].audio = 1;
	t.t[2].lba = 4500 * 61 + 75 * 59 + 74;
	t.t[2].audio = 1;
	t.leadout = 300000;
	k = rc_cue(&t, "Game (SLUS_000.76).bin", cue, sizeof cue);
	CHECK(k > 0 && !strcmp(cue, "FILE \"Game (SLUS_000.76).bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n"
		"  TRACK 02 AUDIO\r\n    INDEX 01 12:34:56\r\n  TRACK 03 AUDIO\r\n    INDEX 01 61:59:74\r\n"), "cue sheet text, minutes:seconds:frames");
	CHECK(rc_cue(&t, "x.bin", cue, 40) == -1, "cue sheet that does not fit says so");

	/* --- the title ID --- */
	make_disc(0);
	rr = rc_disc_id(rd_iso, NULL, id, sizeof id);
	CHECK(rr >= 0 && !strcmp(id, want_id), "title ID from 2048-byte sectors: \"%s\" (the ISO mounted by Linux says \"%s\")", id, want_id);
	rr = rc_disc_id(rd_raw, NULL, id, sizeof id);
	CHECK(rr >= 0 && !strcmp(id, want_id), "title ID through raw 2352-byte sectors: \"%s\"", id);
	{
		uint8_t raw[2352];

		raw_data(16, raw);
		CHECK(rc_user_off(raw) == 24, "a Mode 2 sector's data starts at 24");
		raw[15] = 1;
		CHECK(rc_user_off(raw) == 16, "a Mode 1 sector's data starts at 16");
		raw_audio(dtoc.leadout + 200000, raw);
		raw[0] = 0x12;
		CHECK(rc_user_off(raw) == -1, "a music sector is not taken for data");
	}

	/* --- names --- */
	rc_name(nm, 100, "Grand Theft Auto: San Andreas", "SLUS_209.46", "PS2 disc");
	CHECK(!strcmp(nm, "Grand Theft Auto San Andreas (SLUS_209.46)"), "name: \"%s\"", nm);
	rc_name(nm, 100, NULL, "SLUS_209.46", "PS2 disc");
	CHECK(!strcmp(nm, "SLUS_209.46"), "name without a title: \"%s\"", nm);
	rc_name(nm, 100, "  ??? ", "", "PS1 disc");
	CHECK(!strcmp(nm, "PS1 disc"), "name with nothing usable: \"%s\"", nm);
	rc_name(nm, 100, "Ratchet & Clank: Up Your Arsenal / 100% + more...", "SCUS_973.53", "x");
	CHECK(!strcmp(nm, "Ratchet & Clank Up Your Arsenal 100 more (SCUS_973.53)"), "name: \"%s\"", nm);

	/* --- CD copies --- */
	make_disc(0);
	reset();
	rr = rc_rip_cd(&dtoc, &io, buf, 64, &st);
	CHECK(rr == 0 && expect_same(NULL, "S1") && !st.bad && !st.gap_bad && !fatal && !odd_writes && tries_max == 1,
		"CD, one data track: %u sectors, exact, every sector asked for once", dtoc.leadout);

	make_disc(3);
	reset();
	blank = calloc(dtoc.leadout, 1);
	gaps = 0;
	for (s = data_end; s < dtoc.t[1].lba; s++, gaps++)
		fail_left[s] = blank[s] = 255;		/* the pregap does not read as data */
	for (s = dtoc.leadout - 150; s < dtoc.leadout; s++, gaps++)
		fail_left[s] = blank[s] = 255;		/* nor does the run-out */
	rr = rc_rip_cd(&dtoc, &io, buf, 64, &st);
	{
		uint32_t asked = 0;

		for (s = 0; s < dtoc.leadout; s++)
			asked += blank[s] && tries[s];
		CHECK(rr == 0 && expect_same(blank, "S2") && !st.bad && st.gap_bad == gaps && !fatal && !odd_writes && tries_max == 1 && asked == 16,
			"CD with music, unreadable gaps: exact, %u gap sectors blank, only %u of them asked for (once each), no read with the wrong kind (%u), none past the end (%u)",
			st.gap_bad, asked, audio_on_data, past_end);
	}

	reset();
	memset(blank, 0, dtoc.leadout);
	for (s = data_end; s < dtoc.t[1].lba; s++)
		fail_left[s] = blank[s] = 255;		/* the first gap is dead, the others and the run-out read */
	rr = rc_rip_cd(&dtoc, &io, buf, 64, &st);
	CHECK(rr == 0 && expect_same(blank, "S2b") && !st.bad && st.gap_bad == 150 && !fatal && tries[dtoc.leadout - 1] == 1 && tries[dtoc.t[2].lba - 1] == 1,
		"CD, one dead gap: only that gap is blank; the later gaps and the disc's last sector are read");

	reset();
	memset(blank, 0, dtoc.leadout);
	for (s = dtoc.leadout - 150; s < dtoc.leadout - 146; s++)
		fail_left[s] = blank[s] = 255;		/* 4 dead sectors at the start of the run-out, the rest reads */
	for (s = dtoc.t[2].lba - 100; s < dtoc.t[2].lba - 93; s++)
		fail_left[s] = blank[s] = 255;		/* and 7 inside a gap */
	rr = rc_rip_cd(&dtoc, &io, buf, 64, &st);
	CHECK(rr == 0 && expect_same(blank, "S2c") && !st.bad && st.gap_bad == 11 && !fatal,
		"CD, a few dead sectors inside gaps: only those are blank (%u)", st.gap_bad);

	reset();
	memset(blank, 0, dtoc.leadout);
	for (s = 1000; s < 1005; s++)
		fail_left[s] = 2;
	rr = rc_rip_cd(&dtoc, &io, buf, 64, &st);
	CHECK(rr == 0 && expect_same(NULL, "S3") && !st.bad && st.retries == 5 && !fatal, "CD, 5 sectors that read on the third try: exact, %u second tries", st.retries);

	reset();
	for (s = 2000; s < 2003; s++)
		fail_left[s] = blank[s] = 255;
	rr = rc_rip_cd(&dtoc, &io, buf, 64, &st);
	CHECK(rr == 0 && expect_same(blank, "S4") && st.bad == 3 && st.first_bad == 2000 && tries[2000] == 3 && !fatal,
		"CD, 3 dead sectors: blank in the copy, counted (%u), first %u, tried %u times each", st.bad, st.first_bad, tries[2000]);

	reset();
	stop_at_pct = 50;
	rr = rc_rip_cd(&dtoc, &io, buf, 64, &st);
	CHECK(rr == RC_STOP && out_len < (uint64_t)dtoc.leadout * 2352 * 6 / 10 && !fatal, "CD, stopped half way: says stopped at %llu of %llu bytes",
		(unsigned long long)out_len, (unsigned long long)dtoc.leadout * 2352);

	reset();
	write_fail_at = 3;
	rr = rc_rip_cd(&dtoc, &io, buf, 64, &st);
	CHECK(rr == RC_EWRITE, "CD, the destination stops taking data: says so (%d)", rr);

	reset();
	memset(fail_left, 255, dtoc.leadout);
	rr = rc_rip_cd(&dtoc, &io, buf, 64, &st);
	CHECK(rr == RC_EREAD && reads < 200, "CD, nothing reads: gives up after %u reads (%d)", reads, rr);

	reset();
	for (s = 500; s < 900; s++)
		fail_left[s] = 255;
	rr = rc_rip_cd(&dtoc, &io, buf, 64, &st);
	CHECK(rr == RC_EREAD && st.first_bad == 500, "CD, 400 dead sectors in a row: gives up (%d, %u counted)", rr, st.bad);

	reset();
	fail_left[300] = 255;
	gone_after = 0;
	rr = rc_rip_cd(&dtoc, &io, buf, 64, &st);
	CHECK(rr == RC_GONE, "CD, the disc leaves the drive: says so (%d)", rr);

	CHECK(rc_rip_cd(&dtoc, &io, buf, 60, &st) == RC_EARG, "CD, a buffer that would break the 512-byte write rule is refused");

	/* --- v0.44: CD copies the fast way (many sectors a command, sub-channel bytes after each) --- */
	{
		static uint8_t fbuf[256 * 2368];
		rc_io fio = io;
		uint8_t raw[2352];
		uint32_t asked = 0;

		fio.read_cd_n = d_read_cd_n;
		fio.fast_audio = 1;

		raw_data(700, raw);
		CHECK(rc_data_ok(raw, 700) && !rc_data_ok(raw, 701) && !rc_data_ok(raw, 699), "a data sector is taken only for the address it carries");
		raw[15] = 1;
		CHECK(rc_data_ok(raw, 700), "a Mode 1 sector with the right address is taken");
		raw_audio(dtoc.t[1].lba + 5, raw);
		CHECK(!rc_data_ok(raw, dtoc.t[1].lba + 5), "a music sector is not taken for a data sector");

		make_disc(0);
		reset();
		rr = rc_rip_cd(&dtoc, &fio, fbuf, 256, &st);
		CHECK(rr == 0 && expect_same(NULL, "F1") && st.fast == dtoc.leadout && reads == 0 && fast_cmds == (dtoc.leadout + 255) / 256 && !fatal
			&& !odd_writes && !st.bad, "fast CD, one data track: exact, %u sectors in %u commands, no single read", st.fast, fast_cmds);

		make_disc(3);
		reset();
		rr = rc_rip_cd(&dtoc, &fio, fbuf, 256, &st);
		CHECK(rr == 0 && expect_same(NULL, "F2") && st.fast == dtoc.leadout - 150 && reads == 150 && !st.fast_wrong && !st.bad && !st.gap_bad
			&& !fatal && !odd_writes && tries_max == 1, "fast CD with music: exact, no sub-channel byte in the copy; only the 150 pregap sectors read singly (%u)", reads);

		reset();
		fio.fast_audio = 0;
		rr = rc_rip_cd(&dtoc, &fio, fbuf, 256, &st);
		CHECK(rr == 0 && expect_same(NULL, "F3") && fast_music_secs == 0 && st.fast == data_end && reads == dtoc.leadout - data_end && !fatal
			&& tries_max == 1, "fast CD, music not allowed the fast way: exact, no music sector asked for the fast way (%u), each read singly once", fast_music_secs);
		fio.fast_audio = 1;

		reset();
		memset(blank, 0, dtoc.leadout);
		gaps = 0;
		for (s = data_end; s < dtoc.t[1].lba; s++, gaps++)
			fail_left[s] = blank[s] = 255;
		for (s = dtoc.leadout - 150; s < dtoc.leadout; s++, gaps++)
			fail_left[s] = blank[s] = 255;
		rr = rc_rip_cd(&dtoc, &fio, fbuf, 256, &st);
		for (s = 0; s < dtoc.leadout; s++)
			asked += blank[s] && tries[s];
		CHECK(rr == 0 && expect_same(blank, "F4") && !st.bad && st.gap_bad == gaps && !fatal && tries_max == 1 && asked == 16 && st.fast_fail >= 2,
			"fast CD, unreadable gaps: exact, %u gap sectors blank, %u of them asked for; the %u commands that failed left nothing stale in the copy",
			st.gap_bad, asked, st.fast_fail);

		reset();
		fast_fail_at = 1000;
		rr = rc_rip_cd(&dtoc, &fio, fbuf, 256, &st);
		CHECK(rr == 0 && expect_same(NULL, "F5") && st.fast_fail == 1 && tries[1000] == 1 && tries[768] == 1 && tries[1023] == 1 && tries[1024] == 0
			&& !st.bad && !fatal, "fast CD, one command fails: its 256 sectors are read singly, exact");

		reset();
		fast_wrong_at = 1500;
		rr = rc_rip_cd(&dtoc, &fio, fbuf, 256, &st);
		CHECK(rr == 0 && expect_same(NULL, "F6") && st.fast_wrong == 1 && tries[1500] == 1 && tries[1499] == 0 && !st.bad && !fatal,
			"fast CD, a command gives the neighbour of one data sector: noticed, that sector read again singly, exact");

		reset();
		memset(blank, 0, dtoc.leadout);
		for (s = 2000; s < 2003; s++)
			fail_left[s] = blank[s] = 255;
		rr = rc_rip_cd(&dtoc, &fio, fbuf, 256, &st);
		CHECK(rr == 0 && expect_same(blank, "F7") && st.bad == 3 && st.first_bad == 2000 && tries[2000] == 3 && !fatal,
			"fast CD, 3 dead sectors: blank in the copy, counted (%u), tried %u times each", st.bad, tries[2000]);

		reset();
		fast_fail_all = 1;
		rr = rc_rip_cd(&dtoc, &fio, fbuf, 256, &st);
		CHECK(rr == 0 && expect_same(NULL, "F8") && fast_cmds == 8 && st.fast == 0 && st.fast_off_at && !st.bad && !fatal,
			"fast CD, every command fails: given up after %u commands, the whole disc read singly, exact", fast_cmds);

		reset();
		fast_wrong_all = 1;
		rr = rc_rip_cd(&dtoc, &fio, fbuf, 256, &st);
		CHECK(rr == 0 && expect_same(NULL, "F9") && fast_cmds == 1 && st.fast_off_at && !st.bad && !fatal,
			"fast CD, a drive that gives the wrong sectors: given up after the first command, exact");

		reset();
		stop_at_pct = 50;
		rr = rc_rip_cd(&dtoc, &fio, fbuf, 256, &st);
		CHECK(rr == RC_STOP && out_len < (uint64_t)dtoc.leadout * 2352 * 6 / 10 && !fatal, "fast CD, stopped half way: says stopped");

		CHECK(rc_rip_cd(&dtoc, &fio, fbuf, 2048, &st) == RC_EARG, "fast CD, more sectors a command than IRISMAN asks for is refused");
	}
	free(blank);

	/* --- DVD copies --- */
	make_disc(0);
	dtoc.leadout = iso_sectors;
	reset();
	rr = rc_rip_dvd(iso_sectors, &io, dbuf, 256, &st);
	CHECK(rr == 0 && out_len == (uint64_t)iso_sectors * 2048 && !memcmp(out, iso, out_len) && !st.bad && !fatal && tries_max == 1,
		"DVD: %u sectors, exact, each read once", iso_sectors);

	reset();
	multi_fail_lba = 777;
	rr = rc_rip_dvd(iso_sectors, &io, dbuf, 256, &st);
	CHECK(rr == 0 && out_len == (uint64_t)iso_sectors * 2048 && !memcmp(out, iso, out_len) && !st.bad && !fatal,
		"DVD, a block that only reads sector by sector: exact");

	reset();
	fail_left[1234] = 255;
	rr = rc_rip_dvd(iso_sectors, &io, dbuf, 256, &st);
	{
		int same = out_len == (uint64_t)iso_sectors * 2048 && !memcmp(out, iso, 1234 * 2048)
			&& !memcmp(out + 1235 * 2048, iso + 1235 * 2048, (size_t)(iso_sectors - 1235) * 2048);
		int zero = 1;

		for (k = 0; k < 2048; k++)
			zero &= out[1234 * 2048 + k] == 0;
		CHECK(rr == 0 && same && zero && st.bad == 1 && st.first_bad == 1234, "DVD, one dead sector: blank in the copy, counted, the rest exact");
	}

	reset();
	memset(fail_left, 255, iso_sectors);
	rr = rc_rip_dvd(iso_sectors, &io, dbuf, 256, &st);
	CHECK(rr == RC_EREAD && reads < 400, "DVD, nothing reads: gives up after %u reads (%d)", reads, rr);

	reset();
	stop_at_pct = 30;
	rr = rc_rip_dvd(iso_sectors, &io, dbuf, 256, &st);
	CHECK(rr == RC_STOP && out_len < (uint64_t)iso_sectors * 2048 / 2, "DVD, stopped: says stopped");

	reset();
	fail_left[40] = 255;
	gone_after = 0;
	rr = rc_rip_dvd(iso_sectors, &io, dbuf, 256, &st);
	CHECK(rr == RC_GONE, "DVD, the disc leaves the drive: says so (%d)", rr);
}

/* ---- a file in parts ---- */
#define T_PART	0x1FF800ull	/* 2 MiB less one 2048-byte sector */
static uint8_t *pp_data[20];
static uint64_t pp_len[20];
static int pp_cur, pp_opens, pp_whole, pp_fail_open = -2, pp_closed_bad;
static char pp_dir[400], pp_name[200];

static int pp_open(void *ctx, int part)
{
	(void)ctx;
	if (part == pp_fail_open)
		return -1;
	pp_opens++;
	if (part < 0)
	{
		pp_whole = 1;
		part = 0;
	}
	pp_cur = part;
	free(pp_data[part]);
	pp_data[part] = malloc(T_PART * 4);
	pp_len[part] = 0;
	return 0;
}

static int pp_write(void *ctx, const uint8_t *b, uint32_t len)
{
	(void)ctx;
	if (pp_len[pp_cur] + len > T_PART * 4)
		return -1;
	memcpy(pp_data[pp_cur] + pp_len[pp_cur], b, len);
	pp_len[pp_cur] += len;
	return 0;
}

static int pp_close(void *ctx)
{
	(void)ctx;
	if (pp_dir[0])
	{
		char p[700];
		FILE *f;

		if (pp_whole)
			snprintf(p, sizeof p, "%s/%s", pp_dir, pp_name);
		else
			snprintf(p, sizeof p, "%s/%s.%d", pp_dir, pp_name, pp_cur);
		f = fopen(p, "wb");
		if (!f || fwrite(pp_data[pp_cur], 1, pp_len[pp_cur], f) != pp_len[pp_cur])
			pp_closed_bad++;
		if (f)
			fclose(f);
	}
	return 0;
}

static uint8_t pat(uint64_t i)
{
	return (uint8_t)((i * 2654435761ull) >> 11 ^ i >> 3);
}

/* total bytes of pat() through rc_parts in pieces of `piece` bytes; returns rc_parts_end's answer */
static int parts_run(rc_parts *p, uint64_t total, uint64_t whole_max, uint32_t piece, uint64_t send)
{
	static uint8_t b[600000];
	uint64_t at = 0;
	uint32_t i;
	int k;

	for (k = 0; k < 20; k++)
		pp_len[k] = 0;
	pp_opens = pp_whole = pp_closed_bad = 0;
	memset(p, 0, sizeof *p);
	p->open = pp_open;
	p->write = pp_write;
	p->close = pp_close;
	if (rc_parts_begin(p, total, T_PART, whole_max))
		return -100;
	while (at < send)
	{
		uint32_t n = send - at < piece ? (uint32_t)(send - at) : piece;

		for (i = 0; i < n; i++)
			b[i] = pat(at + i);
		if (rc_parts_write(p, b, n))
			return -101;
		at += n;
	}
	return rc_parts_end(p);
}

static int parts_same(int nparts, uint64_t total)
{
	uint64_t at = 0, i;
	int k;

	for (k = 0; k < nparts; k++)
	{
		if (k + 1 < nparts && pp_len[k] != T_PART)
			return 0;
		for (i = 0; i < pp_len[k]; i++)
			if (pp_data[k][i] != pat(at + i))
				return 0;
		at += pp_len[k];
	}
	return at == total && (nparts == 20 || pp_len[nparts] == 0);
}

static void unit_parts(void)
{
	rc_parts p;
	int r;

	r = parts_run(&p, 5000000, T_PART + 2047, 150528, 5000000);
	CHECK(r == 0 && p.parts == 3 && pp_opens == 3 && !pp_whole && parts_same(3, 5000000),
		"parts: 5,000,000 bytes over a %llu-byte limit -> 3 parts, the first two exactly full, joined = the file", (unsigned long long)T_PART);
	r = parts_run(&p, T_PART * 2, T_PART + 2047, 524288, T_PART * 2);
	CHECK(r == 0 && p.parts == 2 && pp_opens == 2 && parts_same(2, T_PART * 2), "parts: exactly two parts' worth -> 2 parts, no empty third");
	r = parts_run(&p, T_PART + 2047, T_PART + 2047, 99999, T_PART + 2047);
	CHECK(r == 0 && p.parts == 1 && pp_whole && parts_same(1, T_PART + 2047), "parts: a file at the limit stays whole");
	r = parts_run(&p, 5000000, 0, 150528, 5000000);
	CHECK(r == 0 && p.parts == 1 && pp_whole && pp_len[0] == 5000000, "parts: a device with no limit gets one file");
	r = parts_run(&p, T_PART * 16 + 1, T_PART, 524288, 0);
	CHECK(r == -100, "parts: more than 16 parts refused before anything is written (%d opens)", pp_opens);
	pp_fail_open = 1;
	r = parts_run(&p, 5000000, T_PART + 2047, 150528, 5000000);
	pp_fail_open = -2;
	CHECK(r == -101, "parts: a part that cannot be opened stops the copy");
	r = parts_run(&p, 5000000, T_PART + 2047, 150528, 4000000);
	CHECK(r != 0, "parts: a copy that ends short is not called whole (%d)", r);
}

/* ---- exFAT image access (as exw_test.c) ---- */
static FILE *img;

static int x_rd(void *ctx, uint32_t lba, uint32_t n, void *b)
{
	(void)ctx;
	return fseeko(img, (off_t)lba * 512, SEEK_SET) || fread(b, 512, n, img) != n ? -1 : 0;
}

static int x_wr(void *ctx, uint32_t lba, uint32_t n, const void *b)
{
	(void)ctx;
	return fseeko(img, (off_t)lba * 512, SEEK_SET) || fwrite(b, 512, n, img) != n ? -1 : 0;
}

static void save(const char *path, const void *d, size_t n)
{
	FILE *f = fopen(path, "wb");

	if (f)
	{
		fwrite(d, 1, n, f);
		fclose(f);
	}
}

int main(int argc, char **argv)
{
	static uint8_t buf[256 * 2368];
	/* RC_FAST=1: the CD copies of "rip" and "exfat" go the fast way (v0.44) */
	const int fastmode = getenv("RC_FAST") != NULL;
	rc_io cio = io;
	const char *isop = argc > 2 ? argv[argc == 7 ? 5 : 2] : NULL;	/* "exfat" names the ISO fifth */
	rc_stats st;
	FILE *f;
	long n;
	int r;

	if (argc < 3)
		return 2;
	f = fopen(isop, "rb");
	if (!f)
		return 3;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	iso_sectors = (uint32_t)(n / 2048);
	iso = malloc((size_t)iso_sectors * 2048);
	if (fread(iso, 2048, iso_sectors, f) != iso_sectors)
		return 3;
	fclose(f);
	if (fastmode)
	{
		cio.read_cd_n = d_read_cd_n;
		cio.fast_audio = 1;
	}

	if (!strcmp(argv[1], "unit") && argc == 4)
	{
		unit(argv[3]);
		unit_parts();
		printf("---- %d passed, %d failed\n", pass, fail);
		return fail ? 1 : 0;
	}
	if (!strcmp(argv[1], "rip") && argc == 4)
	{
		char p[600], cue[4096];
		uint32_t s;
		int k, i;

		make_disc(3);
		reset();
		for (s = data_end; s < dtoc.t[1].lba; s++)
			fail_left[s] = 255;
		r = rc_rip_cd(&dtoc, &cio, buf, fastmode ? 256 : 64, &st);
		/* (the pregap is dead here: the two commands over it fail and their sectors go singly) */
		if (r || fatal || (fastmode && st.fast < dtoc.leadout / 2))
			return 1;
		snprintf(p, sizeof p, "%s/game.bin", argv[3]);
		save(p, out, out_len);
		k = rc_cue(&dtoc, "game.bin", cue, sizeof cue);
		snprintf(p, sizeof p, "%s/game.cue", argv[3]);
		save(p, cue, (size_t)k);
		for (i = 1; i < dtoc.n; i++)
		{
			uint32_t end = i + 1 < dtoc.n ? dtoc.t[i + 1].lba : dtoc.leadout;
			uint8_t *a = malloc((size_t)(end - dtoc.t[i].lba) * 2352);

			for (s = dtoc.t[i].lba; s < end; s++)
				disc_sector(s, a + (size_t)(s - dtoc.t[i].lba) * 2352);
			snprintf(p, sizeof p, "%s/track%02d.raw", argv[3], i + 1);
			save(p, a, (size_t)(end - dtoc.t[i].lba) * 2352);
			free(a);
		}
		printf("%u data sectors, %d tracks, end %u\n", data_end, dtoc.n, dtoc.leadout);
		return 0;
	}
	if (!strcmp(argv[1], "parts") && argc == 5)
	{
		/* parts <game.iso> <outdir> <name>: the ISO as a split game, small parts, into files */
		rc_parts p;
		uint64_t total = (uint64_t)iso_sectors * 2048, at;

		snprintf(pp_dir, sizeof pp_dir, "%s", argv[3]);
		snprintf(pp_name, sizeof pp_name, "%s", argv[4]);
		memset(&p, 0, sizeof p);
		p.open = pp_open;
		p.write = pp_write;
		p.close = pp_close;
		if (rc_parts_begin(&p, total, T_PART, T_PART + 2047))
			return 1;
		for (at = 0; at < total; at += 150528)
			if (rc_parts_write(&p, iso + at, total - at < 150528 ? (uint32_t)(total - at) : 150528))
				return 1;
		r = rc_parts_end(&p);
		printf("%d parts, end %d, write errors %d\n", p.parts, r, pp_closed_bad);
		return r || pp_closed_bad ? 1 : 0;
	}
	if (!strcmp(argv[1], "exfat") && argc == 7)
	{
		exw_dev d;
		char why[200], p[600];
		int cd = !strcmp(argv[6], "cd");
		uint64_t size;

		make_disc(cd ? 3 : 0);
		if (!cd)
			dtoc.leadout = iso_sectors;
		reset();
		size = cd ? (uint64_t)dtoc.leadout * 2352 : (uint64_t)iso_sectors * 2048;
		/* the bytes as they must come out, from a plain run first */
		r = cd ? rc_rip_cd(&dtoc, &io, buf, 64, &st) : rc_rip_dvd(iso_sectors, &io, buf, 256, &st);
		if (r || out_len != size)
			return 1;
		snprintf(p, sizeof p, "%s.expect", argv[2]);
		save(p, out, out_len);
		img = fopen(argv[2], "r+b");
		if (!img)
			return 3;
		fseeko(img, 0, SEEK_END);
		d.rd = x_rd;
		d.wr = x_wr;
		d.ctx = NULL;
		d.sectors = (uint32_t)(ftello(img) / 512);
		r = exw_begin(&d, argv[3], argv[4], size, why, sizeof why);
		printf("begin %d: %s\n", r, why);
		if (r)
			return 1;
		reset();
		exfat_out = 1;
		r = cd ? rc_rip_cd(&dtoc, &cio, buf, fastmode ? 256 : 64, &st) : rc_rip_dvd(iso_sectors, &io, buf, 256, &st);
		printf("copy %d, writes after a part-sector write: %u, sectors the fast way: %u\n", r, odd_writes, (unsigned)st.fast);
		if (cd && fastmode && st.fast < dtoc.leadout - 300)
			r = -1;
		if (r || odd_writes)
		{
			exw_abort();
			return 1;
		}
		r = exw_finish(why, sizeof why);
		printf("finish %d: %s\n", r, why);
		fclose(img);
		return r ? 1 : 0;
	}
	return 2;
}
