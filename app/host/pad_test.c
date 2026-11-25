/* v0.72: src/pad.c on the PC over a pretend pad (host/fake_pad/io/pad.h).
 *   bash host/pad_test.sh      -> "pad_test: N checks, 0 failed"
 * The pretend pad answers as the console's does: len 0 when it has nothing new since the last call. The
 * test counts presses the way main.c does (held & ~last). With -DOLD_BUTTONS it runs v0.71's buttons()
 * instead, which must FAIL: that is the fault Bananafuzzbuddy saw (one press counted three times). */
#include <stdio.h>
#include <string.h>
#include <io/pad.h>
#include "screens.h"
#include "pad.h"

static int connected[MAX_PADS];
static struct { int fresh; unsigned bits; } now[MAX_PADS];
static int checks, failed;

static void check(int ok, const char *what, int a, int b)
{
	checks++;
	if (!ok)
	{
		failed++;
		printf("FAIL: %s (%d, %d)\n", what, a, b);
	}
}

s32 ioPadGetInfo(padInfo *info)
{
	int i;

	memset(info, 0, sizeof *info);
	for (i = 0; i < MAX_PADS; i++)
		info->status[i] = (u8)connected[i];
	return 0;
}

s32 ioPadGetData(u32 port, padData *d)
{
	memset(d, 0, sizeof *d);
	if (!now[port].fresh)
		return 0;		/* len 0: nothing new */
	d->len = 24;
	d->BTN_CROSS = !!(now[port].bits & PAD_CROSS);
	d->BTN_CIRCLE = !!(now[port].bits & PAD_CIRCLE);
	d->BTN_UP = !!(now[port].bits & PAD_UP);
	d->BTN_DOWN = !!(now[port].bits & PAD_DOWN);
	d->BTN_LEFT = !!(now[port].bits & PAD_LEFT);
	d->BTN_RIGHT = !!(now[port].bits & PAD_RIGHT);
	d->BTN_SQUARE = !!(now[port].bits & PAD_SQUARE);
	d->BTN_TRIANGLE = !!(now[port].bits & PAD_TRIANGLE);
	d->BTN_L1 = !!(now[port].bits & PAD_L1);
	d->BTN_R1 = !!(now[port].bits & PAD_R1);
	d->BTN_L2 = !!(now[port].bits & PAD_L2);
	d->BTN_R2 = !!(now[port].bits & PAD_R2);
	d->BTN_SELECT = !!(now[port].bits & PAD_SELECT);
	d->BTN_START = !!(now[port].bits & PAD_START);
	now[port].fresh = 0;
	return 0;
}

#ifdef OLD_BUTTONS
/* v0.71 main.c, word for word */
static unsigned buttons(void)
{
	padInfo info;
	padData d;
	unsigned b = 0;
	int i;

	if (ioPadGetInfo(&info) != 0)
		return 0;
	for (i = 0; i < MAX_PADS; i++)
	{
		if (!info.status[i] || ioPadGetData(i, &d) != 0 || d.len <= 0)
			continue;
		b |= (d.BTN_UP ? PAD_UP : 0) | (d.BTN_DOWN ? PAD_DOWN : 0) | (d.BTN_LEFT ? PAD_LEFT : 0) | (d.BTN_RIGHT ? PAD_RIGHT : 0)
			| (d.BTN_CROSS ? PAD_CROSS : 0) | (d.BTN_CIRCLE ? PAD_CIRCLE : 0) | (d.BTN_SQUARE ? PAD_SQUARE : 0)
			| (d.BTN_TRIANGLE ? PAD_TRIANGLE : 0) | (d.BTN_L1 ? PAD_L1 : 0) | (d.BTN_R1 ? PAD_R1 : 0)
			| (d.BTN_L2 ? PAD_L2 : 0) | (d.BTN_R2 ? PAD_R2 : 0) | (d.BTN_SELECT ? PAD_SELECT : 0) | (d.BTN_START ? PAD_START : 0);
	}
	return b;
}
#define pad_buttons buttons
#endif

static unsigned last;
static int presses[16], longest_gap_release;

/* one frame of main.c: the pad reports (or not), Prism asks */
static void tick(int pad, unsigned bits, int reports)
{
	unsigned held, pressed;
	int k;

	if (reports)
	{
		now[pad].fresh = 1;
		now[pad].bits = bits;
	}
	held = pad_buttons();
	pressed = held & ~last;
	for (k = 0; k < 16; k++)
		if (pressed & (1u << k))
			presses[k]++;
	if (bits && !held)
		longest_gap_release++;
	last = held;
}

static int count(unsigned bit)
{
	int k, n = 0;

	for (k = 0; k < 16; k++)
		if (bit & (1u << k))
			n += presses[k];
	return n;
}

int main(void)
{
	int f;

	connected[0] = 1;
	/* 1. Cross held for 12 frames at 60 a second while the pad reports every third frame, then let go */
	for (f = 0; f < 12; f++)
		tick(0, PAD_CROSS, f % 3 == 0);
	tick(0, 0, 1);
	for (f = 0; f < 5; f++)
		tick(0, 0, f % 3 == 0);
	check(count(PAD_CROSS) == 1, "one Cross press, held 12 frames with a report every third, counted as", count(PAD_CROSS), 1);
	check(longest_gap_release == 0, "frames where a held button read as let go", longest_gap_release, 0);

	/* 2. two real presses are two */
	memset(presses, 0, sizeof presses);
	for (f = 0; f < 6; f++)
		tick(0, PAD_CIRCLE, f % 3 == 0);
	tick(0, 0, 1);
	tick(0, 0, 0);
	for (f = 0; f < 6; f++)
		tick(0, PAD_CIRCLE, f % 3 == 0);
	tick(0, 0, 1);
	check(count(PAD_CIRCLE) == 2, "two Circle presses counted as", count(PAD_CIRCLE), 2);

	/* 3. a release is seen at once when the pad reports it, and not before */
	memset(presses, 0, sizeof presses);
	tick(0, PAD_UP, 1);
	tick(0, PAD_UP, 0);
	check(last == PAD_UP, "Up still held on a frame with no report", (int)last, PAD_UP);
	tick(0, 0, 1);
	check(last == 0, "Up let go when the pad says so", (int)last, 0);

	/* 4. a second pad adds its buttons; pulling a pad out lets go of what it held */
	memset(presses, 0, sizeof presses);
	connected[1] = 1;
	tick(1, PAD_SQUARE, 1);
	tick(0, PAD_LEFT, 1);
	check(last == (PAD_SQUARE | PAD_LEFT), "two pads together", (int)last, PAD_SQUARE | PAD_LEFT);
	connected[1] = 0;
	tick(0, PAD_LEFT, 0);
	check(last == PAD_LEFT, "a pad pulled out while holding Square still holds it", (int)last, PAD_LEFT);
	connected[1] = 1;
	tick(0, PAD_LEFT, 0);
	check(last == PAD_LEFT, "a pad plugged back in holds what it held before", (int)last, PAD_LEFT);
	tick(0, 0, 1);

	/* 5. every button comes through as its own bit */
	{
		static const unsigned all[14] = { PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE,
			PAD_L1, PAD_R1, PAD_L2, PAD_R2, PAD_SELECT, PAD_START };
		int k;

		for (k = 0; k < 14; k++)
		{
			tick(0, all[k], 1);
			check(last == all[k], "a button came through as another", (int)last, (int)all[k]);
		}
		tick(0, 0, 1);
	}
	printf("pad_test: %d checks, %d failed\n", checks, failed);
	return failed != 0;
}
