/* RAM log + in-place file write + tiny formatter + timebase. */
#include <ps2emu/ps2emu.h>
#include <ps2emu/libc.h>
#include <ps2emu/io.h>
#include <ps2emu/symbols.h>
#include "lr.h"

/* On the heap since 2026-11-21g: as a static array it took 12 KB of the 64 KB the stage2 may
 * occupy. NULL (no memory) = no log. */
char *lr_logbuf;
static u32 lr_loglen;
static u8 lr_log_full;

/* ---------------------------------------------------------------- timebase */

/* mftb r3,268 exactly as gxemu encodes it (0x7d2c42e6 is the r9 form). gxemu re-reads while the
 * low word is 0; same here. */
static u64 __attribute__((noinline)) lr_tb_raw(void)
{
	register u64 v __asm__("3");
	__asm__ volatile(".long 0x7c6c42e6" : "=r"(v));
	return v;
}

u64 lr_tb(void)
{
	u64 v;
	int guard = 64;

	do
		v = lr_tb_raw();
	while ((u32)v == 0 && --guard);

	return v;
}

void lr_udelay(u32 us)
{
	u64 end = lr_tb() + ((u64)us * LR_TB_PER_MS) / 1000 + 1;

	while ((s64)(lr_tb() - end) < 0)
		;
}

void lr_mdelay(u32 ms)
{
	while (ms--)
		lr_udelay(1000);
}

/* --------------------------------------------------------------- formatter */
/* %s %c %d %u %x %X %p, flags: 0, width, l / ll. Always NUL-terminates. Returns length. */

static void put(char *out, int size, int *n, char c)
{
	if (*n + 1 < size)
		out[*n] = c;
	(*n)++;
}

int lr_vfmt(char *out, int size, const char *fmt, va_list ap)
{
	int n = 0;

	while (*fmt)
	{
		char c = *fmt++;

		if (c != '%')
		{
			put(out, size, &n, c);
			continue;
		}

		int zero = 0, width = 0, lng = 0;

		if (*fmt == '0')
		{
			zero = 1;
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		while (*fmt == 'l')
		{
			lng = 1;
			fmt++;
		}

		c = *fmt ? *fmt++ : 0;

		if (c == 's')
		{
			const char *s = va_arg(ap, const char *);
			int l = 0;

			if (!s)
				s = "(null)";
			while (s[l])
				l++;
			for (; width > l; width--)
				put(out, size, &n, ' ');
			while (*s)
				put(out, size, &n, *s++);
		}
		else if (c == 'c')
		{
			put(out, size, &n, (char)va_arg(ap, int));
		}
		else if (c == 'd' || c == 'u' || c == 'x' || c == 'X' || c == 'p')
		{
			u64 v;
			int neg = 0, base = (c == 'd' || c == 'u') ? 10 : 16;
			char tmp[24];
			int l = 0;

			if (c == 'p')
				v = (u64)va_arg(ap, void *);
			else if (c == 'd')
			{
				s64 sv = lng ? va_arg(ap, long) : (s64)va_arg(ap, int);

				if (sv < 0)
				{
					neg = 1;
					sv = -sv;
				}
				v = (u64)sv;
			}
			else
				v = lng ? va_arg(ap, unsigned long) : (u64)va_arg(ap, unsigned int);

			do
			{
				int dgt = (int)(v % base);

				tmp[l++] = (char)(dgt < 10 ? '0' + dgt : (c == 'X' ? 'A' : 'a') + dgt - 10);
				v /= base;
			} while (v && l < 22);

			if (neg)
				tmp[l++] = '-';
			for (; width > l; width--)
				put(out, size, &n, zero ? '0' : ' ');
			while (l)
				put(out, size, &n, tmp[--l]);
		}
		else if (c)
		{
			put(out, size, &n, c);
		}
	}

	if (size > 0)
		out[n < size ? n : size - 1] = 0;

	return n < size ? n : size - 1;
}

int lr_fmt(char *out, int size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = lr_vfmt(out, size, fmt, ap);
	va_end(ap);

	return n;
}

/* --------------------------------------------------------------------- log */

void lr_log(const char *fmt, ...)
{
	char line[200];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = lr_vfmt(line, sizeof(line), fmt, ap);
	va_end(ap);

	if (!lr_logbuf)
	{
		lr_logbuf = malloc(LR_LOG_SZ);
		if (!lr_logbuf)
			return;
		memset(lr_logbuf, 0, LR_LOG_SZ);
	}

	if (lr_loglen + n < LR_LOG_SZ - 8)
	{
		memcpy(lr_logbuf + lr_loglen, line, n);
		lr_loglen += n;
	}
	else if (!lr_log_full)
	{
		/* say so once, in the last 8 bytes that are always kept free */
		memcpy(lr_logbuf + LR_LOG_SZ - 8, "\n#FULL#\n", 8);
		lr_log_full = 1;
	}

	lr_net_send(line, n);
}

u64 lr_file_size(int fd)
{
	u64 *st = (u64 *)((0x1270 * (u64)fd) + *(u64 *)(u64)(TOC + GX_FILETAB_PTR_TOC));

	return st[0x178 / 8];
}

/* The file must already exist and be at least LR_LOG_SZ long: the write is in place and can
 * never grow the file. If it is missing or short, nothing is written. */
void lr_log_flush(void)
{
	static u8 warned;
	int fd;

	if (!lr_logbuf)
		return;
	fd = ufs_open(0, LR_LOG_FILE);

	if (fd < 0)
	{
		if (!warned)
		{
			warned = 1;
			lr_log("log: no %s (%d), file log off\n", LR_LOG_FILE, fd);
		}
		return;
	}

	u64 size = lr_file_size(fd);

	if (size >= LR_LOG_SZ)
		ufs_write(fd, 0, lr_logbuf, LR_LOG_SZ);
	else if (!warned)
	{
		warned = 1;
		lr_log("log: %s is %ld bytes, need %d, file log off\n", LR_LOG_FILE, (long)size, LR_LOG_SZ);
	}

	ufs_close(fd);
}

/* One fixed record past the RAM log's part of the file, overwritten each time: a long session
 * leaves its LATEST statistics there however full the log is (R6: the last line of a
 * 25-minute run was written at 131 s). Needs the 64 KB file bench/lr_bench.py creates. */
void lr_log_tail(const char *fmt, ...)
{
	char rec[LR_TAIL_SZ];
	va_list ap;
	int fd;

	memset(rec, 0, sizeof(rec));
	va_start(ap, fmt);
	lr_vfmt(rec, sizeof(rec) - 1, fmt, ap);
	va_end(ap);

	fd = ufs_open(0, LR_LOG_FILE);
	if (fd < 0)
		return;
	if (lr_file_size(fd) >= LR_TAIL_OFF + LR_TAIL_SZ)
		ufs_write(fd, LR_TAIL_OFF, rec, LR_TAIL_SZ);
	ufs_close(fd);
}
