/* v0.37: writing one new file into an existing folder of the SD card's exFAT volume (exfat_w.c).
 * Plain C over a sector device, so the PC tests (host/exw_test.c) run the same code as the console. */
#ifndef PRISM_APP_EXFAT_W_H
#define PRISM_APP_EXFAT_W_H
#include <stdint.h>

typedef struct
{
	int (*rd)(void *ctx, uint32_t lba, uint32_t n, void *buf);		/* 512-byte sectors; 0 = done */
	int (*wr)(void *ctx, uint32_t lba, uint32_t n, const void *buf);
	void *ctx;
	uint32_t sectors;							/* the whole device */
} exw_dev;

/* Find the first exFAT volume on dev (whole device, or an MBR primary partition) and get ready to
 * add <dir>/<name> of size bytes (dir: one folder under the root, e.g. "PS2ISO"; it must exist;
 * the name must not). Nothing is written. 0 = ready, else why says what stopped it. */
int  exw_begin(const exw_dev *dev, const char *dir, const char *name, uint64_t size, char *why, int wn);
/* The file's bytes, in order; every call but the last a whole number of 512-byte sectors.
 * Written straight into free clusters, which nothing refers to until exw_finish. */
int  exw_write(const void *buf, uint32_t len);
/* Mark the clusters used and add the folder entry. 0 = the file is on the volume. */
int  exw_finish(char *why, int wn);
/* Give up (nothing on the volume refers to what exw_write put there). */
void exw_abort(void);
/* v0.73: delete the file <dir>/<name> (never a folder). Everything is checked before the first write.
 * 0 = gone and its space free; -1 = nothing was changed (why says what stopped it); -2 = the file is
 * gone but a later write failed (why says what a disk check has to tidy). */
int  exw_delete(const exw_dev *dev, const char *dir, const char *name, char *why, int wn);
/* v0.73: the exFAT volume's free and whole size, bytes. 0 = found; nothing is written */
int  exw_space(const exw_dev *dev, uint64_t *free_b, uint64_t *total_b, char *why, int wn);
/* free space on the exFAT volume (after exw_begin or exw_delete), bytes */
uint64_t exw_free_bytes(void);
#endif
