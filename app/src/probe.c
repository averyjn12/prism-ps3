/* Prism app, step 1: a probe with no picture. It reads the SD card in the console's own reader
 * as raw sectors, mounts every FAT32 / exFAT partition with the stage2's own reader (lr_fs.c,
 * unchanged), lists the games, reads known places in two of them, times raw reads, writes it all
 * to /dev_hdd0/tmp/prism_app.log. It writes nothing anywhere else. (START in the app.) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ppu-lv2.h>
#include "fsport.h"
#include "storage.h"

#define APP_VERSION	"probe-2026-11-22b"
#define LOG_PATH	"/dev_hdd0/tmp/prism_app.log"
#define TB_HZ		79800000ULL

static u64 tb(void)
{
	u64 t;

	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

static void hex16(const char *what, u64 off, const u8 *b, int n)
{
	int i;

	lr_log("  %s @%llx:", what, (unsigned long long)off);
	for (i = 0; i < n; i++)
		lr_log(" %02x", b[i]);
	lr_log("\n");
}

/* read len bytes at off of the open file and log them */
static void peek(const char *what, u64 off, int len)
{
	u8 b[64];
	int r;

	memset(b, 0xEE, sizeof(b));
	r = lr_file_read(off, b, (u32)len);
	if (r)
		lr_log("  %s @%llx: read failed %d\n", what, (unsigned long long)off, r);
	else
		hex16(what, off, b, len);
}

static void file_checks(const char *path, const u64 *offs, int n)
{
	u64 size = 0;
	int r = lr_fs_open(path, &size), i;

	lr_log("open %s: %d size %llu\n", path, r, (unsigned long long)size);
	if (r)
		return;
	peek("pvd", 0x8000, 48);
	for (i = 0; i < n; i++)
		peek("at", offs[i] == ~0ULL ? size - 16 : offs[i], 32);
}

static void speed(void)
{
	static u8 big[512 * 512] __attribute__((aligned(128)));
	u32 chunk[3] = { 64, 128, 512 }, k, i, lba0 = 0x2003280;	/* GTASA.ISO's first sector on the card */
	u64 size = 0;

	for (k = 0; k < 3; k++)
	{
		u32 n = chunk[k], reps = (32u << 20) / (n * 512);
		u64 t0 = tb();
		int bad = 0;

		for (i = 0; i < reps; i++)
			bad |= fsport_raw(lba0 + (k + 1) * 0x40000 + i * n, n, big);
		lr_log("speed: raw %u-sector reads, 32 MB in %llu ms%s\n", n, (unsigned long long)((tb() - t0) * 1000 / TB_HZ),
			bad ? " (READ ERRORS)" : "");
	}
	if (lr_fs_open("/PS2ISO/GTASA.ISO", &size) == 0)
	{
		u64 t0 = tb(), off;
		int bad = 0;

		for (off = 0x10000000; off < 0x12000000; off += sizeof(big) / 4)
			bad |= lr_file_read(off, big, sizeof(big) / 4);
		lr_log("speed: lr_file_read 64 KB at a time, 32 MB in %llu ms%s\n", (unsigned long long)((tb() - t0) * 1000 / TB_HZ),
			bad ? " (READ ERRORS)" : "");
	}
}

int run_probe(void)
{
	static const u64 ids[] = { ST_DEV_SD, ST_DEV_MS, ST_DEV_CF, ST_DEV_CARDCTRL, ST_DEV_USB000 };
	static const char *const names[] = { "sd", "ms", "cf", "cardctrl", "usb000" };
	static const u64 gta[] = { ~0ULL };
	static const u64 mgs2[] = { 0xBFFFFFF0ULL, 2196092ULL * 2048, ~0ULL };
	st_devinfo info;
	int i, r;

	fsport_log = fopen(LOG_PATH, "w");
	if (!fsport_log)
		return 1;
	lr_log("prism app %s, stage2 reader %s\n", APP_VERSION, LR_VERSION);

	for (i = 0; i < 5; i++)
	{
		memset(&info, 0, sizeof(info));
		r = st_info(ids[i], &info);
		lr_log("dev %-8s %llx: rc %x name %.7s sectors %llu size %u writable %u\n", names[i], (unsigned long long)ids[i], (u32)r,
			r ? "" : (const char *)info.name, (unsigned long long)info.total_sectors, info.sector_size, info.writable);
	}

	r = fsport_attach(ST_DEV_SD);
	if (r == 0)
	{
		r = lr_fs_mount();
		lr_log("mount: %d\n", r);
		if (r == 0)
		{
			int n = lr_fs_list(), k;
			u8 rec[128];

			lr_log("list: %d game(s)%s\n", n, lr_cat_more ? " (more)" : "");
			for (k = 0; k < n; k++)
			{
				u32 secs;

				lr_fs_cat_rec((u32)k, rec);
				secs = rec[4] | (rec[5] << 8) | (rec[6] << 16) | ((u32)rec[7] << 24);
				lr_log("  game %d: %s, %u sectors\n", k, lr_fs_cat_name((u32)k), secs);
			}
			file_checks("/PS2ISO/GTASA.ISO", gta, 1);
			file_checks("/PS2ISO/Metal Gear Solid 2 - Substance (USA).iso", mgs2, 3);
			speed();
		}
		fsport_detach();
	}
	lr_log("reads %u errors %u\ndone\n", fsport_reads, fsport_errs);
	fclose(fsport_log);
	return 0;
}
