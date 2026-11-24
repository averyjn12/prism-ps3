/* v0.38: the commands Prism sends the disc drive to copy a PS1 / PS2 disc (drive.c). */
#ifndef PRISM_APP_DRIVE_H
#define PRISM_APP_DRIVE_H
#include <ppu-types.h>

#define DRV_DEV		0x101000000000006ULL	/* the BD drive (psdevwiki DeviceID; cobralib BDVD_DRIVE) */
#define DRV_EBUSY	0x8001000A
#define DRV_TOC_MAX	804			/* header + 100 track descriptors */

/* one line to the rip log, written and closed before the call it announces */
void drv_set_log(void (*fn)(const char *fmt, ...));

/* "Reset BD drive": what the Cobra library's cobra_disc_auth() does for a disc that is not a PS3
 * disc, and the only step multiMAN takes before it reads a PS2 DVD. Returns the call's result. */
s32 drv_auth_reset(void);
/* READ TOC: the 4-byte header, then exactly the length it names. *len = bytes in out. */
s32 drv_toc(u32 fd, u8 *out, int *len);
/* READ CD: one raw 2352-byte sector; audio = it lies in an audio track. say = log before the call. */
s32 drv_read_cd(u32 fd, u32 lba, int audio, u8 *buf, int say);
/* v0.44, READ CD the way IRISMAN sends it: n sectors (1 .. DRV_FAST_MAX) with one command, each
 * 2352 raw bytes and then 16 sub-channel bytes; buf holds n * DRV_RAWQ bytes. */
#define DRV_RAWQ	0x940
#define DRV_FAST_MAX	256
s32 drv_read_cd_n(u32 fd, u32 lba, u32 n, u8 *buf, int say);
/* v0.47, SET CD SPEED: the read speed in kB/s (177 = 1x; DRV_SPEED_MAX = the drive's highest) */
#define DRV_SPEED_MAX	0xFFFF
#define DRV_SPEED_4X	708	/* what the drive read CDs at before it was asked (measured 11-23) */
s32 drv_set_cd_speed(u32 fd, u32 kbs);
#endif
