/* lr_fs.c's block layer on the PS3 side: the raw SD card through lv2 storage syscalls
 * (600 open, 601 close, 602 read, 609 device info; argument order as webMAN's rawseciso).
 * Read-only: nothing here can write to the card. */
#include <stdarg.h>
#include <string.h>
#include <ppu-lv2.h>
#include "fsport.h"
#include "storage.h"

static u8 dma_buf[LR_DMA_SZ] __attribute__((aligned(128)));
u8 *lr_dma = dma_buf;
u32 lr_blk_count;
FILE *fsport_log;
static u32 st_fd;
static int st_open_ok;
u32 fsport_reads, fsport_errs;

s32 st_info(u64 dev, st_devinfo *info)
{
	lv2syscall2(609, dev, (u64)info);
	return_to_user_prog(s32);
}

s32 st_open(u64 dev, u32 *fd)
{
	lv2syscall4(600, dev, 0, (u64)fd, 0);
	return_to_user_prog(s32);
}

s32 st_close(u32 fd)
{
	lv2syscall1(601, fd);
	return_to_user_prog(s32);
}

s32 st_read(u32 fd, u32 lba, u32 n, void *buf, u32 *got)
{
	lv2syscall7(602, fd, 0, lba, n, (u64)buf, (u64)got, 0);
	return_to_user_prog(s32);
}

int fsport_attach(u64 dev)
{
	st_devinfo info;
	s32 r;

	memset(&info, 0, sizeof(info));
	r = st_info(dev, &info);
	if (r)
	{
		lr_log("st: device %llx info rc %x\n", (unsigned long long)dev, (u32)r);
		return LR_E_NOMEDIUM;
	}
	if (info.sector_size != 512 || !info.total_sectors || info.total_sectors >> 32)
	{
		lr_log("st: device %llx sector size %u, %llu sectors: not usable\n", (unsigned long long)dev, info.sector_size,
			(unsigned long long)info.total_sectors);
		return LR_E_STATE;
	}
	r = st_open(dev, &st_fd);
	if (r)
	{
		lr_log("st: open %llx rc %x\n", (unsigned long long)dev, (u32)r);
		return LR_E_STATE;
	}
	st_open_ok = 1;
	lr_blk_count = (u32)info.total_sectors;
	lr_log("st: device %llx open, %u sectors of 512\n", (unsigned long long)dev, lr_blk_count);
	return 0;
}

void fsport_detach(void)
{
	if (st_open_ok)
		st_close(st_fd);
	st_open_ok = 0;
	lr_blk_count = 0;
}

/* raw read into any buffer (speed tests) */
int fsport_raw(u32 lba, u32 n, void *buf)
{
	u32 got = 0;
	s32 r = st_read(st_fd, lba, n, buf, &got);

	return (r || got != n) ? LR_E_XACT : 0;
}

int lr_blk_read(u32 lba, u32 n, u32 off)
{
	int tries;

	if (!st_open_ok || !n || lba >= lr_blk_count || n > lr_blk_count - lba || off + n * 512 > LR_DMA_SZ)
		return LR_E_STATE;

	for (tries = 0; tries < 3; tries++)
	{
		u32 got = 0;
		s32 r = st_read(st_fd, lba, n, lr_dma + off, &got);

		fsport_reads++;
		if (r == 0 && got == n)
			return 0;
		fsport_errs++;
		lr_log("st: read lba %x n %u rc %x got %u\n", lba, n, (u32)r, got);
	}
	return LR_E_XACT;
}

void lr_log(const char *fmt, ...)
{
	va_list ap;

	if (!fsport_log)
		return;
	va_start(ap, fmt);
	vfprintf(fsport_log, fmt, ap);
	va_end(ap);
	fflush(fsport_log);
}

int lr_fmt(char *out, int size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(out, (size_t)size, fmt, ap);
	va_end(ap);
	if (n < 0)
		n = 0;
	return n < size ? n : size - 1;
}
