/* v0.72: the pads, as PAD_* bits (screens.h). Out of main.c so the PC can test it (host/pad_test.c).
 *
 * Bananafuzzbuddy, 11-25, v0.71: "I'll press cross and it's like I hit enter 3 times", and one Circle that left the
 * PS2 list and then Prism. ioPadGetData() answers len 0 when the pad has sent nothing new since the last
 * call. buttons() in main.c took that as "no button is down", so a button held through such a frame was
 * released and pressed again. Up to v0.66 Prism asked 20 times a second and nearly every answer was new;
 * at 60 it asks more often than the pad reports. A pad with nothing new still holds what it held. */
#include <io/pad.h>
#include "screens.h"
#include "pad.h"

static unsigned was[MAX_PADS];

unsigned pad_buttons(void)
{
	padInfo info;
	padData d;
	unsigned b = 0;
	int i;

	if (ioPadGetInfo(&info) != 0)
		return 0;
	for (i = 0; i < MAX_PADS; i++)
	{
		if (!info.status[i])
		{
			was[i] = 0;	/* unplugged: nothing is held */
			continue;
		}
		if (ioPadGetData(i, &d) == 0 && d.len > 0)
			was[i] = (d.BTN_UP ? PAD_UP : 0) | (d.BTN_DOWN ? PAD_DOWN : 0) | (d.BTN_LEFT ? PAD_LEFT : 0) | (d.BTN_RIGHT ? PAD_RIGHT : 0)
				| (d.BTN_CROSS ? PAD_CROSS : 0) | (d.BTN_CIRCLE ? PAD_CIRCLE : 0) | (d.BTN_SQUARE ? PAD_SQUARE : 0)
				| (d.BTN_TRIANGLE ? PAD_TRIANGLE : 0) | (d.BTN_L1 ? PAD_L1 : 0) | (d.BTN_R1 ? PAD_R1 : 0)
				| (d.BTN_L2 ? PAD_L2 : 0) | (d.BTN_R2 ? PAD_R2 : 0) | (d.BTN_SELECT ? PAD_SELECT : 0) | (d.BTN_START ? PAD_START : 0);
		b |= was[i];
	}
	return b;
}
