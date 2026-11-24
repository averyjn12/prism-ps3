/* v0.38: the parts of a PS1 / PS2 disc copy that do not touch the console: the table of contents,
 * the cue sheet, the title ID, the file name, and the two copy loops over callbacks. Plain C, so
 * host/ripcore_test.c runs the same code against a pretend drive (ripcore.c). */
#ifndef PRISM_APP_RIPCORE_H
#define PRISM_APP_RIPCORE_H
#include <stdint.h>

#define RC_MAX_TRACKS	99
#define RC_RAW		2352u

typedef struct
{
	uint32_t lba;		/* where the track starts (its INDEX 01) */
	uint8_t audio;
} rc_track;

typedef struct
{
	int n;
	rc_track t[RC_MAX_TRACKS];
	uint32_t leadout;	/* the sector after the last one */
} rc_toc;

/* results of the copy loops */
#define RC_OK		0
#define RC_STOP		(-9)	/* the player stopped it */
#define RC_EWRITE	(-4)
#define RC_EREAD	(-3)	/* the disc is not being read at all, or far too much of it is not */
#define RC_GONE		(-8)	/* the disc left the drive */
#define RC_EARG		(-1)

typedef struct
{
	/* one raw 2352-byte sector; audio = the sector lies in an audio track. 0 = read */
	int (*read_cd)(void *ctx, uint32_t lba, int audio, uint8_t *buf);
	/* n 2048-byte sectors. 0 = all read */
	int (*read_2048)(void *ctx, uint32_t lba, uint32_t n, uint8_t *buf);
	/* the image's next bytes, in order. 0 = written */
	int (*write)(void *ctx, const uint8_t *buf, uint32_t len);
	/* called between reads; 1 = stop asked */
	int (*tick)(void *ctx, uint64_t done, uint64_t total);
	/* 0 = no disc in the drive any more (may be NULL) */
	int (*present)(void *ctx);
	/* one line for the log (may be NULL) */
	void (*log)(void *ctx, const char *line);
	void *ctx;
	/* v0.44, the fast way (may be NULL): n raw sectors with one command, each followed by its 16
	 * sub-channel bytes - n * RC_RAWQ bytes, as IRISMAN reads a PS1 disc. 0 = the command worked.
	 * Every sector of the data track that comes this way is checked (rc_data_ok); one that does
	 * not look right, and every sector of a command that failed, is read with read_cd as before. */
	int (*read_cd_n)(void *ctx, uint32_t lba, uint32_t n, uint8_t *buf);
	int fast_audio;		/* 1 = music sectors may come through read_cd_n too */
} rc_io;

typedef struct
{
	uint32_t bad;		/* sectors that could not be read and are blank in the copy */
	uint32_t gap_bad;	/* the same, in the two seconds before a track start or the disc's end, where that is usual */
	uint32_t first_bad;	/* the first of the `bad` ones */
	uint32_t retries;	/* reads that worked only when tried again */
	uint64_t done;		/* bytes written */
	uint32_t fast;		/* sectors that came through read_cd_n */
	uint32_t fast_fail;	/* read_cd_n commands that failed (their sectors were read one by one) */
	uint32_t fast_wrong;	/* data sectors read_cd_n gave that did not look right (read again one by one) */
	uint32_t fast_off_at;	/* the sector at which read_cd_n was given up for the rest of the disc, or 0 */
} rc_stats;

/* The answer to READ TOC/PMA/ATIP format 0 (header + 8-byte track descriptors, lead-out = 0xAA) as
 * a track list. 0 = fine; negative = not a layout Prism copies (says why). */
int rc_toc_parse(const uint8_t *resp, int len, rc_toc *out, char *why, int wn);
/* 1 = lba lies in an audio track */
int rc_audio_at(const rc_toc *toc, uint32_t lba);
/* the cue sheet for <bin> (text, CR LF lines); returns its length, or -1 if it does not fit */
int rc_cue(const rc_toc *toc, const char *bin, char *out, int n);
/* where a raw sector's 2048 bytes of user data start: 16 (Mode 1), 24 (Mode 2), or -1 = no data sector */
int rc_user_off(const uint8_t *raw);

/* 1 = raw is the data sector that belongs at lba: the sync pattern, Mode 1 or 2, and the sector's own
 * address (minutes, seconds, frames in BCD, 150 ahead of lba) */
int rc_data_ok(const uint8_t *raw, uint32_t lba);

/* SYSTEM.CNF's boot file through an ISO 9660 volume read by rd (one 2048-byte sector per call).
 * Returns 1 = PS2 (BOOT2), 0 = PS1 (BOOT), negative = none found. id gets "SLUS_209.46" or "". */
typedef int (*rc_rd2048)(void *ctx, uint32_t lba, uint8_t *buf);
int rc_disc_id(rc_rd2048 rd, void *ctx, char *id, int idn);

/* a file name (no extension) from the game's title and ID: "Title (SLUS_209.46)", or the ID, or
 * fallback. Only characters every file system here takes; at most max bytes. */
void rc_name(char *out, int max, const char *title, const char *id, const char *fallback);

/* Copy a CD as raw sectors 0 .. lead-out: buf holds chunk sectors (a multiple of 32, so that every
 * write but the last is whole 512-byte sectors; at most RC_CHUNK_MAX) - chunk * RC_RAW bytes, or
 * chunk * RC_RAWQ when io->read_cd_n is set. Unreadable sectors are written blank and counted. */
#define RC_RAWQ		2368u	/* a raw sector and its 16 sub-channel bytes */
#define RC_CHUNK_MAX	1024u	/* IRISMAN's largest read */
int rc_rip_cd(const rc_toc *toc, const rc_io *io, uint8_t *buf, uint32_t chunk, rc_stats *st);
/* Copy sectors 0 .. sectors of a DVD (2048 bytes each), chunk sectors a read. */
int rc_rip_dvd(uint32_t sectors, const rc_io *io, uint8_t *buf, uint32_t chunk, rc_stats *st);

/* A file that may have to be written as parts (FAT32 holds no file of 4 GiB or more): "<name>" when
 * it fits whole, else "<name>.0", "<name>.1" ... each part_max bytes but the last - the form Prism's
 * card reader joins (lr_fs.c; every part but the last a whole number of 2048-byte sectors). */
#define RC_PART_MAX	0xFFFF0000ull	/* the part size Cobra's and webMAN's split images use */
#define RC_WHOLE_MAX	0xFFFFFFFFull	/* the largest file FAT32 holds */
#define RC_PARTS_MAX	16		/* what lr_fs.c joins */
typedef struct
{
	int (*open)(void *ctx, int part);	/* part -1 = the file whole; 0 .. = that part. 0 = open */
	int (*write)(void *ctx, const uint8_t *buf, uint32_t len);
	int (*close)(void *ctx);		/* the part that is open. 0 = closed and whole */
	void *ctx;
	uint64_t part_max, total, done, in_part;
	int split, part, is_open, parts;	/* parts: how many were opened */
} rc_parts;
/* whole_max: the largest file the device holds whole (RC_WHOLE_MAX on FAT32; 0 = any size).
 * 0 = ready (nothing opened yet); negative = it would take more than RC_PARTS_MAX parts */
int rc_parts_begin(rc_parts *p, uint64_t total, uint64_t part_max, uint64_t whole_max);
int rc_parts_write(rc_parts *p, const uint8_t *buf, uint32_t len);
int rc_parts_end(rc_parts *p);		/* closes what is open; 0 = every byte of total went out */
#endif
