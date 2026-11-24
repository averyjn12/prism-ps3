/* RSX set-up (display buffers only, no shaders) and the canvas -> screen copy. */
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <sysutil/video.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include "gfx.h"

#define CB_SIZE		0x100000
#define HOST_SIZE	(32 * 1024 * 1024)
#define LABEL		255

static gcmContextData *ctx;
static void *host;
static u32 *vbuf[2], voff[2];
static int cur, dw, dh;
static u16 *xmap;		/* display column -> canvas column, when scaling */
u32 *fb;

static void wait_label(u32 v)
{
	rsxSetWriteBackendLabel(ctx, LABEL, v);
	rsxFlushBuffer(ctx);
	while (*(vu32 *)gcmGetLabelAddress(LABEL) != v)
		usleep(30);
}

static void wait_flip(void)
{
	while (gcmGetFlipStatus() != 0)
		usleep(200);
	gcmResetFlipStatus();
}

static void flip(int i)
{
	if (gcmSetFlip(ctx, i) == 0)
	{
		rsxFlushBuffer(ctx);
		gcmSetWaitFlip(ctx);
	}
}

int gfx_init(void)
{
	videoState st;
	videoResolution res;
	videoConfiguration vc;
	int i;

	host = memalign(1024 * 1024, HOST_SIZE);
	if (!host)
		return -1;
	rsxInit(&ctx, CB_SIZE, HOST_SIZE, host);
	if (!ctx)
		return -2;
	if (videoGetState(0, 0, &st) != 0 || st.state != 0)
		return -3;
	if (videoGetResolution(st.displayMode.resolution, &res) != 0)
		return -4;

	memset(&vc, 0, sizeof(vc));
	vc.resolution = st.displayMode.resolution;
	vc.format = VIDEO_BUFFER_FORMAT_XRGB;
	vc.pitch = res.width * sizeof(u32);
	vc.aspect = st.displayMode.aspect;
	wait_label(1);
	if (videoConfigure(0, &vc, NULL, 0) != 0)
		return -5;
	gcmSetFlipMode(GCM_FLIP_VSYNC);

	dw = res.width;
	dh = res.height;
	for (i = 0; i < 2; i++)
	{
		vbuf[i] = (u32 *)rsxMemalign(64, dw * dh * 4);
		if (!vbuf[i] || rsxAddressToOffset(vbuf[i], &voff[i]) != 0)
			return -6;
		memset(vbuf[i], 0, dw * dh * 4);
		if (gcmSetDisplayBuffer(i, voff[i], dw * 4, dw, dh) != 0)
			return -7;
	}
	fb = (u32 *)memalign(128, CW * CH * 4);
	if (!fb)
		return -8;
	memset(fb, 0, CW * CH * 4);
	if (dw != CW || dh != CH)
	{
		xmap = (u16 *)malloc(dw * sizeof(u16));
		if (!xmap)
			return -9;
		for (i = 0; i < dw; i++)
			xmap[i] = (u16)(i * CW / dw);
	}
	gcmResetFlipStatus();
	flip(1);
	cur = 0;
	return 0;
}

void gfx_present(void)
{
	wait_flip();
	if (!xmap)
		memcpy(vbuf[cur], fb, CW * CH * 4);
	else
	{
		int x, y;

		for (y = 0; y < dh; y++)
		{
			const u32 *s = fb + (y * CH / dh) * CW;
			u32 *d = vbuf[cur] + y * dw;

			for (x = 0; x < dw; x++)
				d[x] = s[xmap[x]];
		}
	}
	flip(cur);
	cur ^= 1;
}

void gfx_end(void)
{
	int i;

	if (!ctx)
		return;
	wait_flip();
	gcmSetWaitFlip(ctx);
	for (i = 0; i < 2; i++)
		if (vbuf[i])
			rsxFree(vbuf[i]);
	rsxFinish(ctx, 1);
	ctx = NULL;
}
