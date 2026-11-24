/* v0.37: the SD card's raw sectors for the exFAT writer (exfat_w.c), through the lv2 storage calls
 * fsport_ps3.c reads the card with (609 info, 600 open, 601 close, 602 read) and 603 write (argument
 * order as 602). Every transfer goes through a 128-byte-aligned buffer, as fsport_ps3.c's reads do.
 * The writer itself refuses any write outside the card's exFAT volume. */
#include <string.h>
#include <malloc.h>
#include <ppu-lv2.h>
#include "storage.h"
#include "exfat_w.h"
#include "exfat_card.h"

#define BOUNCE	128	/* sectors per transfer (64 KB) */

static u32 fd;
static int open_ok;
static u8 *bounce;
u32 exc_writes, exc_errs;

static s32 st_write(u32 h, u32 lba, u32 n, const void *buf, u32 *done)
{
	lv2syscall7(603, h, 0, lba, n, (u64)(unsigned long)buf, (u64)(unsigned long)done, 0);
	return_to_user_prog(s32);
}

static int c_rd(void *ctx, uint32_t lba, uint32_t n, void *buf)
{
	u8 *p = (u8 *)buf;

	(void)ctx;
	while (n)
	{
		u32 k = n < BOUNCE ? n : BOUNCE, got = 0;

		if (st_read(fd, lba, k, bounce, &got) != 0 || got != k)
		{
			exc_errs++;
			return -1;
		}
		memcpy(p, bounce, k * 512);
		p += k * 512;
		lba += k;
		n -= k;
	}
	return 0;
}

static int c_wr(void *ctx, uint32_t lba, uint32_t n, const void *buf)
{
	const u8 *p = (const u8 *)buf;

	(void)ctx;
	while (n)
	{
		u32 k = n < BOUNCE ? n : BOUNCE, done = 0;

		memcpy(bounce, p, k * 512);
		if (st_write(fd, lba, k, bounce, &done) != 0 || done != k)
		{
			exc_errs++;
			return -1;
		}
		exc_writes++;
		p += k * 512;
		lba += k;
		n -= k;
	}
	return 0;
}

int exc_open(exw_dev *d, char *why, int wn)
{
	st_devinfo info;
	s32 r;

	exc_close();
	memset(&info, 0, sizeof info);
	r = st_info(ST_DEV_SD, &info);
	if (r || info.sector_size != 512 || !info.total_sectors || info.total_sectors >> 32)
	{
		if (why && wn > 0)
			snprintf(why, wn, "the card slot did not answer (%x)", (u32)r);
		return -1;
	}
	if (!bounce)
		bounce = (u8 *)memalign(128, BOUNCE * 512);
	if (!bounce)
		return -2;
	r = st_open(ST_DEV_SD, &fd);
	if (r)
	{
		if (why && wn > 0)
			snprintf(why, wn, "could not open the card (%x)", (u32)r);
		return -3;
	}
	open_ok = 1;
	d->rd = c_rd;
	d->wr = c_wr;
	d->ctx = NULL;
	d->sectors = (uint32_t)info.total_sectors;
	return 0;
}

void exc_close(void)
{
	if (open_ok)
		st_close(fd);
	open_ok = 0;
}
