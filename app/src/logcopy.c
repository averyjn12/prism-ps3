/* v0.81: Settings' "Write logs, copy to USB" (Bananafuzzbuddy, 2026-11-26, for the first CECHA tester: his Prism stops at
 * "One moment, Reading the card", and the logs that say why sit in /dev_hdd0/tmp, where he cannot reach them).
 * After the probe log (probe.c) and a fresh console check (check.c), every Prism log in /dev_hdd0/tmp
 * (prism_*.log, prism_*.cfg: the A01 helper's prism_a01.log among them) and webMAN's onxmb.bat go to the folder
 * PRISM_LOGS on the first USB drive found, with a README.txt saying what Prism found on the console.
 * It only reads on the hard disk and only writes in that one folder of the USB drive. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <lv2/sysfs.h>
#include "dirlist.h"
#include "check.h"
#include "version.h"
#include "logcopy.h"

#define SRC_DIR		"/dev_hdd0/tmp"
#define ONXMB		"/dev_hdd0/onxmb.bat"
#define DST_NAME	"PRISM_LOGS"
#define MAX_COPY	(4ULL << 20)	/* a larger file is listed in the README, not copied */

static int ends(const char *s, const char *tail)
{
	size_t a = strlen(s), b = strlen(tail);

	return a >= b && !strcmp(s + a - b, tail);
}

/* 0, or why not: -1 no source, -2 no target, -3 a write failed, -4 the target did not close */
static int copy_file(const char *from, const char *to)
{
	static unsigned char buf[64 * 1024];
	s32 in, out;
	u64 n = 0, w = 0, done = 0;
	int r = 0;

	if (sysFsOpen(from, SYS_O_RDONLY, &in, NULL, 0) != 0)
		return -1;
	if (sysFsOpen(to, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &out, NULL, 0) != 0)
	{
		sysFsClose(in);
		return -2;
	}
	while (done < MAX_COPY && sysFsRead(in, buf, sizeof buf, &n) == 0 && n > 0)
	{
		if (sysFsWrite(out, buf, n, &w) != 0 || w != n)
		{
			r = -3;
			break;
		}
		done += n;
	}
	sysFsClose(in);
	if (sysFsClose(out) != 0 && r == 0)
		r = -4;
	return r;
}

int logs_to_usb(char *msg, int size)
{
	char dev[16], dst[48], from[320], to[320];
	dl_ent *e = NULL;
	sysFSStat st;
	int i, n, r, copied = 0, failed = 0, usb = -1;
	FILE *f;

	for (i = 0; i < 8 && usb < 0; i++)
	{
		snprintf(dev, sizeof dev, "/dev_usb%03d", i);
		if (sysFsStat(dev, &st) == 0)
			usb = i;
	}
	if (usb < 0)
	{
		snprintf(msg, size, "Logs written. No USB drive found: put one in, try again");
		return -1;
	}
	snprintf(dst, sizeof dst, "%s/" DST_NAME, dev);
	sysFsMkdir(dst, 0777);
	if (sysFsStat(dst, &st) != 0 || (st.st_mode & 0170000) != 0040000)	/* a FILE of that name is no folder */
	{
		snprintf(msg, size, "Logs written, but USB %d took no folder (FAT32?)", usb);
		return -2;
	}

	snprintf(to, sizeof to, "%s/README.txt", dst);
	f = fopen(to, "w");
	if (f)
	{
		fprintf(f, "Prism %s: logs for the Prism developer. Send this whole PRISM_LOGS folder.\n\n", APP_VERSION);
		fprintf(f, "model      %s (sub-code %d)\n", model_text(chk.model), chk.model);
		fprintf(f, "firmware   %s, %s\n", chk.fw, chk.cobra ? "Cobra" : "no Cobra");
		fprintf(f, "cobra      %x.%x\n", chk.cobra_bcd >> 8, (chk.cobra_bcd >> 4) & 15);
		fprintf(f, "ps2 emu    %s (its fingerprint is in prism_check.log)\n",
			chk.emu > 0 ? "the one Prism was built for" : chk.emu == 0 ? "a DIFFERENT one" : "missing");
		fprintf(f, "ps2 card   %s %s\n", chk.ps2_ok ? "ready" : "not ready:", chk.ps2_ok ? "" : chk.why);
		fprintf(f, "webMAN     %s\n\nfiles:\n", chk.wm ? "answering" : "not answering");
	}

	n = dir_list(SRC_DIR, &e);
	for (i = 0; i < n; i++)
	{
		if (e[i].dir || strncmp(e[i].name, "prism_", 6) || !(ends(e[i].name, ".log") || ends(e[i].name, ".cfg")))
			continue;
		if (e[i].size > MAX_COPY)
		{
			if (f)
				fprintf(f, "  %-32s %10llu bytes, too big: not copied\n", e[i].name, (unsigned long long)e[i].size);
			continue;
		}
		snprintf(from, sizeof from, SRC_DIR "/%s", e[i].name);
		snprintf(to, sizeof to, "%s/%s", dst, e[i].name);
		r = copy_file(from, to);
		if (r)
			failed++;
		else
			copied++;
		if (f)
			fprintf(f, "  %-32s %10llu bytes, %s\n", e[i].name, (unsigned long long)e[i].size, r ? "COPY FAILED" : "copied");
	}
	free(e);

	/* webMAN's onxmb.bat: whether Prism's setup lines are in it */
	if (sysFsStat(ONXMB, &st) == 0)
	{
		snprintf(to, sizeof to, "%s/onxmb.bat.txt", dst);
		r = copy_file(ONXMB, to);
		if (r)
			failed++;
		else
			copied++;
		if (f)
			fprintf(f, "  %-32s %10llu bytes, %s\n", "onxmb.bat (as onxmb.bat.txt)", (unsigned long long)st.st_size, r ? "COPY FAILED" : "copied");
	}
	if (f)
		fclose(f);

	snprintf(msg, size, "%d logs copied to USB %d, folder " DST_NAME "%s", copied, usb, failed ? " (some failed)" : "");
	return copied;
}
