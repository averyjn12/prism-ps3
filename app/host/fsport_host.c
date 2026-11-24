/* lr_fs.c's block layer on the PC: a raw card image file. Same interface as fsport_ps3.c, so the
 * reader the app runs is tested here against the Linux-made cards (emu/cards, raw-expanded). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "fsport.h"

static u8 dma_buf[LR_DMA_SZ];
u8 *lr_dma = dma_buf;
u32 lr_blk_count;
FILE *fsport_log;
static FILE *img;
u32 fsport_reads, fsport_errs;

int fsport_attach_file(const char *path)
{
	img = fopen(path, "rb");
	if (!img)
		return LR_E_NOMEDIUM;
	fseek(img, 0, SEEK_END);
	lr_blk_count = (u32)(ftell(img) / 512);
	return 0;
}

int lr_blk_read(u32 lba, u32 n, u32 off)
{
	fsport_reads++;
	if (!img || !n || lba >= lr_blk_count || n > lr_blk_count - lba || off + n * 512 > LR_DMA_SZ)
		return LR_E_STATE;
	if (fseek(img, (long)lba * 512, SEEK_SET) || fread(lr_dma + off, 512, n, img) != n)
	{
		fsport_errs++;
		return LR_E_XACT;
	}
	return 0;
}

void lr_log(const char *fmt, ...)
{
	va_list ap;

	if (!fsport_log)
		return;
	va_start(ap, fmt);
	vfprintf(fsport_log, fmt, ap);
	va_end(ap);
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
