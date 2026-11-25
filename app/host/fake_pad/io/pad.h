/* v0.72: the PC stand-in for PSL1GHT's <io/pad.h>, for host/pad_test.c: only what src/pad.c uses */
#ifndef FAKE_PAD_H
#define FAKE_PAD_H
#include <ppu-types.h>
#define MAX_PADS	7
typedef struct { u32 max, connected, info; u16 vendor_id[MAX_PADS], product_id[MAX_PADS]; u8 status[MAX_PADS]; } padInfo;
typedef struct
{
	s32 len;
	unsigned BTN_LEFT : 1, BTN_DOWN : 1, BTN_RIGHT : 1, BTN_UP : 1, BTN_START : 1, BTN_R3 : 1, BTN_L3 : 1, BTN_SELECT : 1;
	unsigned BTN_SQUARE : 1, BTN_CROSS : 1, BTN_CIRCLE : 1, BTN_TRIANGLE : 1, BTN_R1 : 1, BTN_L1 : 1, BTN_R2 : 1, BTN_L2 : 1;
} padData;
s32 ioPadGetInfo(padInfo *info);
s32 ioPadGetData(u32 port, padData *data);
#endif
